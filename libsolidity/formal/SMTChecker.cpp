/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <libsolidity/formal/SMTChecker.h>

#include <libsolidity/formal/SMTPortfolio.h>

#include <libsolidity/formal/VariableUsage.h>
#include <libsolidity/formal/SymbolicTypes.h>

#include <liblangutil/ErrorReporter.h>

#include <libdevcore/StringUtils.h>

#include <boost/range/adaptor/map.hpp>
#include <boost/algorithm/string/replace.hpp>

using namespace std;
using namespace dev;
using namespace langutil;
using namespace dev::solidity;

SMTChecker::SMTChecker(ErrorReporter& _errorReporter, map<h256, string> const& _smtlib2Responses):
	m_interface(make_shared<smt::SMTPortfolio>(_smtlib2Responses)),
	m_errorReporter(_errorReporter)
{
#if defined (HAVE_Z3) || defined (HAVE_CVC4)
	if (!_smtlib2Responses.empty())
		m_errorReporter.warning(
			"SMT-LIB2 query responses were given in the auxiliary input, "
			"but this Solidity binary uses an SMT solver (Z3/CVC4) directly."
			"These responses will be ignored."
			"Consider disabling Z3/CVC4 at compilation time in order to use SMT-LIB2 responses."
		);
#endif
}

void SMTChecker::analyze(SourceUnit const& _source, shared_ptr<Scanner> const& _scanner)
{
	m_variableUsage = make_shared<VariableUsage>(_source);
	m_scanner = _scanner;
	if (_source.annotation().experimentalFeatures.count(ExperimentalFeature::SMTChecker))
		_source.accept(*this);
}

bool SMTChecker::visit(ContractDefinition const& _contract)
{
	for (auto _var : _contract.stateVariables())
		createVariable(*_var);
	return true;
}

void SMTChecker::endVisit(ContractDefinition const&)
{
	m_variables.clear();
}

void SMTChecker::endVisit(VariableDeclaration const& _varDecl)
{
	if (_varDecl.isLocalVariable() && _varDecl.type()->isValueType() &&_varDecl.value())
		assignment(_varDecl, *_varDecl.value(), _varDecl.location());
}

bool SMTChecker::visit(FunctionDefinition const& _function)
{
	if (!_function.modifiers().empty() || _function.isConstructor())
		m_errorReporter.warning(
			_function.location(),
			"Assertion checker does not yet support constructors and functions with modifiers."
		);
	m_functionPath.push_back(&_function);
	// Not visited by a function call
	if (isRootFunction())
	{
		m_interface->reset();
		m_pathConditions.clear();
		m_expressions.clear();
		m_globalContext.clear();
		m_uninterpretedTerms.clear();
		resetStateVariables();
		initializeLocalVariables(_function);
		m_loopExecutionHappened = false;
		m_arrayAssignmentHappened = false;
	}

	return true;
}

void SMTChecker::endVisit(FunctionDefinition const&)
{
	// If _function was visited from a function call we don't remove
	// the local variables just yet, since we might need them for
	// future calls.
	// Otherwise we remove any local variables from the context and
	// keep the state variables.
	if (isRootFunction())
		removeLocalVariables();
	m_functionPath.pop_back();
}

bool SMTChecker::visit(IfStatement const& _node)
{
	_node.condition().accept(*this);

	// We ignore called functions here because they have
	// specific input values.
	if (isRootFunction())
		checkBooleanNotConstant(_node.condition(), "Condition is always $VALUE.");

	auto indicesEndTrue = visitBranch(_node.trueStatement(), expr(_node.condition()));
	vector<VariableDeclaration const*> touchedVariables = m_variableUsage->touchedVariables(_node.trueStatement());
	decltype(indicesEndTrue) indicesEndFalse;
	if (_node.falseStatement())
	{
		indicesEndFalse = visitBranch(*_node.falseStatement(), !expr(_node.condition()));
		touchedVariables += m_variableUsage->touchedVariables(*_node.falseStatement());
	}
	else
		indicesEndFalse = copyVariableIndices();

	mergeVariables(touchedVariables, expr(_node.condition()), indicesEndTrue, indicesEndFalse);

	return false;
}

// Here we consider the execution of two branches:
// Branch 1 assumes the loop condition to be true and executes the loop once,
// after resetting touched variables.
// Branch 2 assumes the loop condition to be false and skips the loop after
// visiting the condition (it might contain side-effects, they need to be considered)
// and does not erase knowledge.
// If the loop is a do-while, condition side-effects are lost since the body,
// executed once before the condition, might reassign variables.
// Variables touched by the loop are merged with Branch 2.
bool SMTChecker::visit(WhileStatement const& _node)
{
	auto indicesBeforeLoop = copyVariableIndices();
	auto touchedVariables = m_variableUsage->touchedVariables(_node);
	resetVariables(touchedVariables);
	decltype(indicesBeforeLoop) indicesAfterLoop;
	if (_node.isDoWhile())
	{
		indicesAfterLoop = visitBranch(_node.body());
		// TODO the assertions generated in the body should still be active in the condition
		_node.condition().accept(*this);
		if (isRootFunction())
			checkBooleanNotConstant(_node.condition(), "Do-while loop condition is always $VALUE.");
	}
	else
	{
		_node.condition().accept(*this);
		if (isRootFunction())
			checkBooleanNotConstant(_node.condition(), "While loop condition is always $VALUE.");

		indicesAfterLoop = visitBranch(_node.body(), expr(_node.condition()));
	}

	// We reset the execution to before the loop
	// and visit the condition in case it's not a do-while.
	// A do-while's body might have non-precise information
	// in its first run about variables that are touched.
	resetVariableIndices(indicesBeforeLoop);
	if (!_node.isDoWhile())
		_node.condition().accept(*this);

	mergeVariables(touchedVariables, expr(_node.condition()), indicesAfterLoop, copyVariableIndices());

	m_loopExecutionHappened = true;
	return false;
}

// Here we consider the execution of two branches similar to WhileStatement.
bool SMTChecker::visit(ForStatement const& _node)
{
	if (_node.initializationExpression())
		_node.initializationExpression()->accept(*this);

	auto indicesBeforeLoop = copyVariableIndices();

	// Do not reset the init expression part.
	auto touchedVariables =
		m_variableUsage->touchedVariables(_node.body());
	if (_node.condition())
		touchedVariables += m_variableUsage->touchedVariables(*_node.condition());
	if (_node.loopExpression())
		touchedVariables += m_variableUsage->touchedVariables(*_node.loopExpression());
	// Remove duplicates
	std::sort(touchedVariables.begin(), touchedVariables.end());
	touchedVariables.erase(std::unique(touchedVariables.begin(), touchedVariables.end()), touchedVariables.end());

	resetVariables(touchedVariables);

	if (_node.condition())
	{
		_node.condition()->accept(*this);
		if (isRootFunction())
			checkBooleanNotConstant(*_node.condition(), "For loop condition is always $VALUE.");
	}

	m_interface->push();
	if (_node.condition())
		m_interface->addAssertion(expr(*_node.condition()));
	_node.body().accept(*this);
	if (_node.loopExpression())
		_node.loopExpression()->accept(*this);
	m_interface->pop();

	auto indicesAfterLoop = copyVariableIndices();
	// We reset the execution to before the loop
	// and visit the condition.
	resetVariableIndices(indicesBeforeLoop);
	if (_node.condition())
		_node.condition()->accept(*this);

	auto forCondition = _node.condition() ? expr(*_node.condition()) : smt::Expression(true);
	mergeVariables(touchedVariables, forCondition, indicesAfterLoop, copyVariableIndices());

	m_loopExecutionHappened = true;
	return false;
}

void SMTChecker::endVisit(VariableDeclarationStatement const& _varDecl)
{
	if (_varDecl.declarations().size() != 1)
		m_errorReporter.warning(
			_varDecl.location(),
			"Assertion checker does not yet support such variable declarations."
		);
	else if (knownVariable(*_varDecl.declarations()[0]))
	{
		if (_varDecl.initialValue())
			assignment(*_varDecl.declarations()[0], *_varDecl.initialValue(), _varDecl.location());
	}
	else
		m_errorReporter.warning(
			_varDecl.location(),
			"Assertion checker does not yet implement such variable declarations."
		);
}

void SMTChecker::endVisit(Assignment const& _assignment)
{
	if (_assignment.assignmentOperator() != Token::Assign)
		m_errorReporter.warning(
			_assignment.location(),
			"Assertion checker does not yet implement compound assignment."
		);
	else if (!isSupportedType(_assignment.annotation().type->category()))
		m_errorReporter.warning(
			_assignment.location(),
			"Assertion checker does not yet implement type " + _assignment.annotation().type->toString()
		);
	else if (Identifier const* identifier = dynamic_cast<Identifier const*>(&_assignment.leftHandSide()))
	{
		VariableDeclaration const& decl = dynamic_cast<VariableDeclaration const&>(*identifier->annotation().referencedDeclaration);
		solAssert(knownVariable(decl), "");
		assignment(decl, _assignment.rightHandSide(), _assignment.location());
		defineExpr(_assignment, expr(_assignment.rightHandSide()));
	}
	else if (dynamic_cast<IndexAccess const*>(&_assignment.leftHandSide()))
	{
		arrayIndexAssignment(_assignment);
		defineExpr(_assignment, expr(_assignment.rightHandSide()));
	}
	else
		m_errorReporter.warning(
			_assignment.location(),
			"Assertion checker does not yet implement such assignments."
		);
}

void SMTChecker::endVisit(TupleExpression const& _tuple)
{
	if (
		_tuple.isInlineArray() ||
		_tuple.components().size() != 1 ||
		!isSupportedType(_tuple.components()[0]->annotation().type->category())
	)
		m_errorReporter.warning(
			_tuple.location(),
			"Assertion checker does not yet implement tuples and inline arrays."
		);
	else
		defineExpr(_tuple, expr(*_tuple.components()[0]));
}

void SMTChecker::checkUnderOverflow(smt::Expression _value, IntegerType const& _type, SourceLocation const& _location)
{
	checkCondition(
		_value < minValue(_type),
		_location,
		"Underflow (resulting value less than " + formatNumberReadable(_type.minValue()) + ")",
		"<result>",
		&_value
	);
	checkCondition(
		_value > maxValue(_type),
		_location,
		"Overflow (resulting value larger than " + formatNumberReadable(_type.maxValue()) + ")",
		"<result>",
		&_value
	);
}

void SMTChecker::endVisit(UnaryOperation const& _op)
{
	switch (_op.getOperator())
	{
	case Token::Not: // !
	{
		solAssert(isBool(_op.annotation().type->category()), "");
		defineExpr(_op, !expr(_op.subExpression()));
		break;
	}
	case Token::Inc: // ++ (pre- or postfix)
	case Token::Dec: // -- (pre- or postfix)
	{

		solAssert(isInteger(_op.annotation().type->category()), "");
		solAssert(_op.subExpression().annotation().lValueRequested, "");
		if (Identifier const* identifier = dynamic_cast<Identifier const*>(&_op.subExpression()))
		{
			VariableDeclaration const& decl = dynamic_cast<VariableDeclaration const&>(*identifier->annotation().referencedDeclaration);
			if (knownVariable(decl))
			{
				auto innerValue = currentValue(decl);
				auto newValue = _op.getOperator() == Token::Inc ? innerValue + 1 : innerValue - 1;
				assignment(decl, newValue, _op.location());
				defineExpr(_op, _op.isPrefixOperation() ? newValue : innerValue);
			}
			else
				m_errorReporter.warning(
					_op.location(),
					"Assertion checker does not yet implement such assignments."
				);
		}
		else
			m_errorReporter.warning(
				_op.location(),
				"Assertion checker does not yet implement such increments / decrements."
			);
		break;
	}
	case Token::Sub: // -
	{
		defineExpr(_op, 0 - expr(_op.subExpression()));
		if (auto intType = dynamic_cast<IntegerType const*>(_op.annotation().type.get()))
			checkUnderOverflow(expr(_op), *intType, _op.location());
		break;
	}
	default:
		m_errorReporter.warning(
			_op.location(),
			"Assertion checker does not yet implement this operator."
		);
	}
}

void SMTChecker::endVisit(BinaryOperation const& _op)
{
	if (TokenTraits::isArithmeticOp(_op.getOperator()))
		arithmeticOperation(_op);
	else if (TokenTraits::isCompareOp(_op.getOperator()))
		compareOperation(_op);
	else if (TokenTraits::isBooleanOp(_op.getOperator()))
		booleanOperation(_op);
	else
		m_errorReporter.warning(
			_op.location(),
			"Assertion checker does not yet implement this operator."
		);
}

void SMTChecker::endVisit(FunctionCall const& _funCall)
{
	solAssert(_funCall.annotation().kind != FunctionCallKind::Unset, "");
	if (_funCall.annotation().kind != FunctionCallKind::FunctionCall)
	{
		m_errorReporter.warning(
			_funCall.location(),
			"Assertion checker does not yet implement this expression."
		);
		return;
	}

	FunctionType const& funType = dynamic_cast<FunctionType const&>(*_funCall.expression().annotation().type);

	std::vector<ASTPointer<Expression const>> const args = _funCall.arguments();
	switch (funType.kind())
	{
	case FunctionType::Kind::Assert:
		visitAssert(_funCall);
		break;
	case FunctionType::Kind::Require:
		visitRequire(_funCall);
		break;
	case FunctionType::Kind::GasLeft:
		visitGasLeft(_funCall);
		break;
	case FunctionType::Kind::Internal:
		inlineFunctionCall(_funCall);
		break;
	case FunctionType::Kind::KECCAK256:
	case FunctionType::Kind::ECRecover:
	case FunctionType::Kind::SHA256:
	case FunctionType::Kind::RIPEMD160:
	case FunctionType::Kind::BlockHash:
	case FunctionType::Kind::AddMod:
	case FunctionType::Kind::MulMod:
		abstractFunctionCall(_funCall);
		break;
	default:
		m_errorReporter.warning(
			_funCall.location(),
			"Assertion checker does not yet implement this type of function call."
		);
	}
}

void SMTChecker::visitAssert(FunctionCall const& _funCall)
{
	auto const& args = _funCall.arguments();
	solAssert(args.size() == 1, "");
	solAssert(args[0]->annotation().type->category() == Type::Category::Bool, "");
	checkCondition(!(expr(*args[0])), _funCall.location(), "Assertion violation");
	addPathImpliedExpression(expr(*args[0]));
}

void SMTChecker::visitRequire(FunctionCall const& _funCall)
{
	auto const& args = _funCall.arguments();
	solAssert(args.size() == 1, "");
	solAssert(args[0]->annotation().type->category() == Type::Category::Bool, "");
	if (isRootFunction())
		checkBooleanNotConstant(*args[0], "Condition is always $VALUE.");
	addPathImpliedExpression(expr(*args[0]));
}

void SMTChecker::visitGasLeft(FunctionCall const& _funCall)
{
	string gasLeft = "gasleft()";
	// We increase the variable index since gasleft changes
	// inside a tx.
	defineGlobalVariable(gasLeft, _funCall, true);
	auto const& symbolicVar = m_globalContext.at(gasLeft);
	unsigned index = symbolicVar->index();
	// We set the current value to unknown anyway to add type constraints.
	setUnknownValue(*symbolicVar);
	if (index > 0)
		m_interface->addAssertion(symbolicVar->currentValue() <= symbolicVar->valueAtIndex(index - 1));
}

void SMTChecker::eraseArrayKnowledge()
{
	for (auto const& var: m_variables)
		if (var.first->annotation().type->category() == Type::Category::Mapping)
			newValue(*var.first);
}

void SMTChecker::inlineFunctionCall(FunctionCall const& _funCall)
{
	FunctionDefinition const* _funDef = nullptr;
	Expression const* _calledExpr = &_funCall.expression();

	if (TupleExpression const* _fun = dynamic_cast<TupleExpression const*>(&_funCall.expression()))
	{
		solAssert(_fun->components().size() == 1, "");
		_calledExpr = _fun->components().at(0).get();
	}

	if (Identifier const* _fun = dynamic_cast<Identifier const*>(_calledExpr))
		_funDef = dynamic_cast<FunctionDefinition const*>(_fun->annotation().referencedDeclaration);
	else if (MemberAccess const* _fun = dynamic_cast<MemberAccess const*>(_calledExpr))
		_funDef = dynamic_cast<FunctionDefinition const*>(_fun->annotation().referencedDeclaration);
	else
	{
		m_errorReporter.warning(
			_funCall.location(),
			"Assertion checker does not yet implement this type of function call."
		);
		return;
	}
	solAssert(_funDef, "");

	if (visitedFunction(_funDef))
		m_errorReporter.warning(
			_funCall.location(),
			"Assertion checker does not support recursive function calls.",
			SecondarySourceLocation().append("Starting from function:", _funDef->location())
		);
	else if (_funDef && _funDef->isImplemented())
	{
		vector<smt::Expression> funArgs;
		auto const& funType = dynamic_cast<FunctionType const*>(_calledExpr->annotation().type.get());
		solAssert(funType, "");
		if (funType->bound())
		{
			auto const& boundFunction = dynamic_cast<MemberAccess const*>(_calledExpr);
			solAssert(boundFunction, "");
			funArgs.push_back(expr(boundFunction->expression()));
		}
		for (auto arg: _funCall.arguments())
			funArgs.push_back(expr(*arg));
		initializeFunctionCallParameters(*_funDef, funArgs);
		_funDef->accept(*this);
		auto const& returnParams = _funDef->returnParameters();
		if (_funDef->returnParameters().size())
		{
			if (returnParams.size() > 1)
				m_errorReporter.warning(
					_funCall.location(),
					"Assertion checker does not yet support calls to functions that return more than one value."
				);
			else
				defineExpr(_funCall, currentValue(*returnParams[0]));
		}
	}
	else
	{
		m_errorReporter.warning(
			_funCall.location(),
			"Assertion checker does not support calls to functions without implementation."
		);
	}
}

void SMTChecker::abstractFunctionCall(FunctionCall const& _funCall)
{
	vector<smt::Expression> smtArguments;
	for (auto const& arg: _funCall.arguments())
		smtArguments.push_back(expr(*arg));
	defineExpr(_funCall, (*m_expressions.at(&_funCall.expression()))(smtArguments));
	m_uninterpretedTerms.insert(&_funCall);
	setSymbolicUnknownValue(expr(_funCall), _funCall.annotation().type, *m_interface);
}

void SMTChecker::endVisit(Identifier const& _identifier)
{
	if (_identifier.annotation().lValueRequested)
	{
		// Will be translated as part of the node that requested the lvalue.
	}
	else if (dynamic_cast<FunctionType const*>(_identifier.annotation().type.get()))
	{
		visitFunctionIdentifier(_identifier);
	}
	else if (isSupportedType(_identifier.annotation().type->category()))
	{
		if (VariableDeclaration const* decl = dynamic_cast<VariableDeclaration const*>(_identifier.annotation().referencedDeclaration))
			defineExpr(_identifier, currentValue(*decl));
		else if (_identifier.name() == "now")
			defineGlobalVariable(_identifier.name(), _identifier);
		else
			// TODO: handle MagicVariableDeclaration here
			m_errorReporter.warning(
				_identifier.location(),
				"Assertion checker does not yet support the type of this variable."
			);
	}
}

void SMTChecker::visitFunctionIdentifier(Identifier const& _identifier)
{
	auto const& fType = dynamic_cast<FunctionType const&>(*_identifier.annotation().type);
	if (fType.returnParameterTypes().size() > 1)
	{
		m_errorReporter.warning(
			_identifier.location(),
			"Assertion checker does not yet support functions with more than one return parameter."
		);
	}
	defineGlobalFunction(fType.richIdentifier(), _identifier);
	m_expressions.emplace(&_identifier, m_globalContext.at(fType.richIdentifier()));
}

void SMTChecker::endVisit(Literal const& _literal)
{
	Type const& type = *_literal.annotation().type;
	if (isNumber(type.category()))

		defineExpr(_literal, smt::Expression(type.literalValue(&_literal)));
	else if (isBool(type.category()))
		defineExpr(_literal, smt::Expression(_literal.token() == Token::TrueLiteral ? true : false));
	else
		m_errorReporter.warning(
			_literal.location(),
			"Assertion checker does not yet support the type of this literal (" +
			_literal.annotation().type->toString() +
			")."
		);
}

void SMTChecker::endVisit(Return const& _return)
{
	if (knownExpr(*_return.expression()))
	{
		auto returnParams = m_functionPath.back()->returnParameters();
		if (returnParams.size() > 1)
			m_errorReporter.warning(
				_return.location(),
				"Assertion checker does not yet support more than one return value."
			);
		else if (returnParams.size() == 1)
			m_interface->addAssertion(expr(*_return.expression()) == newValue(*returnParams[0]));
	}
}

bool SMTChecker::visit(MemberAccess const& _memberAccess)
{
	auto const& accessType = _memberAccess.annotation().type;
	if (accessType->category() == Type::Category::Function)
		return true;

	auto const& exprType = _memberAccess.expression().annotation().type;
	solAssert(exprType, "");
	if (exprType->category() == Type::Category::Magic)
	{
		auto identifier = dynamic_cast<Identifier const*>(&_memberAccess.expression());
		string accessedName;
		if (identifier)
			accessedName = identifier->name();
		else
			m_errorReporter.warning(
				_memberAccess.location(),
				"Assertion checker does not yet support this expression."
			);
		defineGlobalVariable(accessedName + "." + _memberAccess.memberName(), _memberAccess);
		return false;
	}
	else
		m_errorReporter.warning(
			_memberAccess.location(),
			"Assertion checker does not yet support this expression."
		);

	return true;
}

void SMTChecker::endVisit(IndexAccess const& _indexAccess)
{
	shared_ptr<SymbolicVariable> array;
	if (auto const& id = dynamic_cast<Identifier const*>(&_indexAccess.baseExpression()))
	{
		auto const& varDecl = dynamic_cast<VariableDeclaration const&>(*id->annotation().referencedDeclaration);
		solAssert(knownVariable(varDecl), "");
		array = m_variables[&varDecl];
	}
	else if (auto const& innerAccess = dynamic_cast<IndexAccess const*>(&_indexAccess.baseExpression()))
	{
		solAssert(knownExpr(*innerAccess), "");
		array = m_expressions[innerAccess];
	}
	else
	{
		m_errorReporter.warning(
			_indexAccess.location(),
			"Assertion checker does not yet implement this expression."
		);
		return;
	}

	solAssert(array, "");
	defineExpr(_indexAccess, smt::Expression::select(
		array->currentValue(),
		expr(*_indexAccess.indexExpression())
	));
	setSymbolicUnknownValue(
		expr(_indexAccess),
		_indexAccess.annotation().type,
		*m_interface
	);
	m_uninterpretedTerms.insert(&_indexAccess);
}

void SMTChecker::arrayAssignment()
{
	m_arrayAssignmentHappened = true;
	eraseArrayKnowledge();
}

void SMTChecker::arrayIndexAssignment(Assignment const& _assignment)
{
	auto const& indexAccess = dynamic_cast<IndexAccess const&>(_assignment.leftHandSide());
	if (auto const& id = dynamic_cast<Identifier const*>(&indexAccess.baseExpression()))
	{
		auto const& varDecl = dynamic_cast<VariableDeclaration const&>(*id->annotation().referencedDeclaration);
		solAssert(knownVariable(varDecl), "");
		smt::Expression store = smt::Expression::store(
			m_variables[&varDecl]->currentValue(),
			expr(*indexAccess.indexExpression()),
			expr(_assignment.rightHandSide())
		);
		m_interface->addAssertion(newValue(varDecl) == store);
	}
	else if (dynamic_cast<IndexAccess const*>(&indexAccess.baseExpression()))
		m_errorReporter.warning(
			indexAccess.location(),
			"Assertion checker does not yet implement assignments to multi-dimensional mappings or arrays."
		);
	else
		m_errorReporter.warning(
			_assignment.location(),
			"Assertion checker does not yet implement this expression."
		);
}

void SMTChecker::defineGlobalVariable(string const& _name, Expression const& _expr, bool _increaseIndex)
{
	if (!knownGlobalSymbol(_name))
	{
		auto result = newSymbolicVariable(*_expr.annotation().type, _name, *m_interface);
		m_globalContext.emplace(_name, result.second);
		setUnknownValue(*result.second);
		if (result.first)
			m_errorReporter.warning(
				_expr.location(),
				"Assertion checker does not yet support this global variable."
			);
	}
	else if (_increaseIndex)
		m_globalContext.at(_name)->increaseIndex();
	// The default behavior is not to increase the index since
	// most of the global values stay the same throughout a tx.
	if (isSupportedType(_expr.annotation().type->category()))
		defineExpr(_expr, m_globalContext.at(_name)->currentValue());
}

void SMTChecker::defineGlobalFunction(string const& _name, Expression const& _expr)
{
	if (!knownGlobalSymbol(_name))
	{
		auto result = newSymbolicVariable(*_expr.annotation().type, _name, *m_interface);
		m_globalContext.emplace(_name, result.second);
		if (result.first)
			m_errorReporter.warning(
				_expr.location(),
				"Assertion checker does not yet support the type of this function."
			);
	}
}

void SMTChecker::arithmeticOperation(BinaryOperation const& _op)
{
	switch (_op.getOperator())
	{
	case Token::Add:
	case Token::Sub:
	case Token::Mul:
	case Token::Div:
	{
		solAssert(_op.annotation().commonType, "");
		if (_op.annotation().commonType->category() != Type::Category::Integer)
		{
			m_errorReporter.warning(
				_op.location(),
				"Assertion checker does not yet implement this operator on non-integer types."
			);
			break;
		}
		auto const& intType = dynamic_cast<IntegerType const&>(*_op.annotation().commonType);
		smt::Expression left(expr(_op.leftExpression()));
		smt::Expression right(expr(_op.rightExpression()));
		Token op = _op.getOperator();
		smt::Expression value(
			op == Token::Add ? left + right :
			op == Token::Sub ? left - right :
			op == Token::Div ? division(left, right, intType) :
			/*op == Token::Mul*/ left * right
		);

		if (_op.getOperator() == Token::Div)
		{
			checkCondition(right == 0, _op.location(), "Division by zero", "<result>", &right);
			m_interface->addAssertion(right != 0);
		}

		checkUnderOverflow(value, intType, _op.location());

		defineExpr(_op, value);
		break;
	}
	default:
		m_errorReporter.warning(
			_op.location(),
			"Assertion checker does not yet implement this operator."
		);
	}
}

void SMTChecker::compareOperation(BinaryOperation const& _op)
{
	solAssert(_op.annotation().commonType, "");
	if (isSupportedType(_op.annotation().commonType->category()))
	{
		smt::Expression left(expr(_op.leftExpression()));
		smt::Expression right(expr(_op.rightExpression()));
		Token op = _op.getOperator();
		shared_ptr<smt::Expression> value;
		if (isNumber(_op.annotation().commonType->category()))
		{
			value = make_shared<smt::Expression>(
				op == Token::Equal ? (left == right) :
				op == Token::NotEqual ? (left != right) :
				op == Token::LessThan ? (left < right) :
				op == Token::LessThanOrEqual ? (left <= right) :
				op == Token::GreaterThan ? (left > right) :
				/*op == Token::GreaterThanOrEqual*/ (left >= right)
			);
		}
		else // Bool
		{
			solUnimplementedAssert(isBool(_op.annotation().commonType->category()), "Operation not yet supported");
			value = make_shared<smt::Expression>(
				op == Token::Equal ? (left == right) :
				/*op == Token::NotEqual*/ (left != right)
			);
		}
		// TODO: check that other values for op are not possible.
		defineExpr(_op, *value);
	}
	else
		m_errorReporter.warning(
			_op.location(),
			"Assertion checker does not yet implement the type " + _op.annotation().commonType->toString() + " for comparisons"
		);
}

void SMTChecker::booleanOperation(BinaryOperation const& _op)
{
	solAssert(_op.getOperator() == Token::And || _op.getOperator() == Token::Or, "");
	solAssert(_op.annotation().commonType, "");
	if (_op.annotation().commonType->category() == Type::Category::Bool)
	{
		// @TODO check that both of them are not constant
		if (_op.getOperator() == Token::And)
			defineExpr(_op, expr(_op.leftExpression()) && expr(_op.rightExpression()));
		else
			defineExpr(_op, expr(_op.leftExpression()) || expr(_op.rightExpression()));
	}
	else
		m_errorReporter.warning(
			_op.location(),
			"Assertion checker does not yet implement the type " + _op.annotation().commonType->toString() + " for boolean operations"
					);
}

smt::Expression SMTChecker::division(smt::Expression _left, smt::Expression _right, IntegerType const& _type)
{
	// Signed division in SMTLIB2 rounds differently for negative division.
	if (_type.isSigned())
		return (smt::Expression::ite(
			_left >= 0,
			smt::Expression::ite(_right >= 0, _left / _right, 0 - (_left / (0 - _right))),
			smt::Expression::ite(_right >= 0, 0 - ((0 - _left) / _right), (0 - _left) / (0 - _right))
		));
	else
		return _left / _right;
}

void SMTChecker::assignment(VariableDeclaration const& _variable, Expression const& _value, SourceLocation const& _location)
{
	assignment(_variable, expr(_value), _location);
}

void SMTChecker::assignment(VariableDeclaration const& _variable, smt::Expression const& _value, SourceLocation const& _location)
{
	TypePointer type = _variable.type();
	if (auto const* intType = dynamic_cast<IntegerType const*>(type.get()))
		checkUnderOverflow(_value, *intType, _location);
	else if (dynamic_cast<AddressType const*>(type.get()))
		checkUnderOverflow(_value, IntegerType(160), _location);
	else if (dynamic_cast<MappingType const*>(type.get()))
		arrayAssignment();
	m_interface->addAssertion(newValue(_variable) == _value);
}

SMTChecker::VariableIndices SMTChecker::visitBranch(Statement const& _statement, smt::Expression _condition)
{
	return visitBranch(_statement, &_condition);
}

SMTChecker::VariableIndices SMTChecker::visitBranch(Statement const& _statement, smt::Expression const* _condition)
{
	auto indicesBeforeBranch = copyVariableIndices();
	if (_condition)
		pushPathCondition(*_condition);
	_statement.accept(*this);
	if (_condition)
		popPathCondition();
	auto indicesAfterBranch = copyVariableIndices();
	resetVariableIndices(indicesBeforeBranch);
	return indicesAfterBranch;
}

void SMTChecker::checkCondition(
	smt::Expression _condition,
	SourceLocation const& _location,
	string const& _description,
	string const& _additionalValueName,
	smt::Expression* _additionalValue
)
{
	m_interface->push();
	addPathConjoinedExpression(_condition);

	vector<smt::Expression> expressionsToEvaluate;
	vector<string> expressionNames;
	if (m_functionPath.size())
	{
		solAssert(m_scanner, "");
		if (_additionalValue)
		{
			expressionsToEvaluate.emplace_back(*_additionalValue);
			expressionNames.push_back(_additionalValueName);
		}
		for (auto const& var: m_variables)
		{
			if (var.first->type()->isValueType())
			{
				expressionsToEvaluate.emplace_back(currentValue(*var.first));
				expressionNames.push_back(var.first->name());
			}
		}
		for (auto const& var: m_globalContext)
		{
			auto const& type = var.second->type();
			if (
				type->isValueType() &&
				smtKind(type->category()) != smt::Kind::Function
			)
			{
				expressionsToEvaluate.emplace_back(var.second->currentValue());
				expressionNames.push_back(var.first);
			}
		}
		for (auto const& uf: m_uninterpretedTerms)
		{
			if (uf->annotation().type->isValueType())
			{
				expressionsToEvaluate.emplace_back(expr(*uf));
				expressionNames.push_back(m_scanner->sourceAt(uf->location()));
			}
		}
	}
	smt::CheckResult result;
	vector<string> values;
	tie(result, values) = checkSatisfiableAndGenerateModel(expressionsToEvaluate);

	string loopComment;
	if (m_loopExecutionHappened)
		loopComment =
			"\nNote that some information is erased after the execution of loops.\n"
			"You can re-introduce information using require().";
	if (m_arrayAssignmentHappened)
		loopComment +=
			"\nNote that array aliasing is not supported,"
			" therefore all mapping information is erased after"
			" a mapping local variable/parameter is assigned.\n"
			"You can re-introduce information using require().";

	switch (result)
	{
	case smt::CheckResult::SATISFIABLE:
	{
		std::ostringstream message;
		message << _description << " happens here";
		if (m_functionPath.size())
		{
			std::ostringstream modelMessage;
			modelMessage << "  for:\n";
			solAssert(values.size() == expressionNames.size(), "");
			map<string, string> sortedModel;
			for (size_t i = 0; i < values.size(); ++i)
				if (expressionsToEvaluate.at(i).name != values.at(i))
					sortedModel[expressionNames.at(i)] = values.at(i);

			for (auto const& eval: sortedModel)
				modelMessage << "  " << eval.first << " = " << eval.second << "\n";
			m_errorReporter.warning(_location, message.str(), SecondarySourceLocation().append(modelMessage.str(), SourceLocation()).append(loopComment, SourceLocation()));
		}
		else
		{
			message << ".";
			m_errorReporter.warning(_location, message.str(), SecondarySourceLocation().append(loopComment, SourceLocation()));
		}
		break;
	}
	case smt::CheckResult::UNSATISFIABLE:
		break;
	case smt::CheckResult::UNKNOWN:
		m_errorReporter.warning(_location, _description + " might happen here.", SecondarySourceLocation().append(loopComment, SourceLocation()));
		break;
	case smt::CheckResult::CONFLICTING:
		m_errorReporter.warning(_location, "At least two SMT solvers provided conflicting answers. Results might not be sound.");
		break;
	case smt::CheckResult::ERROR:
		m_errorReporter.warning(_location, "Error trying to invoke SMT solver.");
		break;
	}
	m_interface->pop();
}

void SMTChecker::checkBooleanNotConstant(Expression const& _condition, string const& _description)
{
	// Do not check for const-ness if this is a constant.
	if (dynamic_cast<Literal const*>(&_condition))
		return;

	m_interface->push();
	addPathConjoinedExpression(expr(_condition));
	auto positiveResult = checkSatisfiable();
	m_interface->pop();

	m_interface->push();
	addPathConjoinedExpression(!expr(_condition));
	auto negatedResult = checkSatisfiable();
	m_interface->pop();

	if (positiveResult == smt::CheckResult::ERROR || negatedResult == smt::CheckResult::ERROR)
		m_errorReporter.warning(_condition.location(), "Error trying to invoke SMT solver.");
	else if (positiveResult == smt::CheckResult::CONFLICTING || negatedResult == smt::CheckResult::CONFLICTING)
		m_errorReporter.warning(_condition.location(), "At least two SMT solvers provided conflicting answers. Results might not be sound.");
	else if (positiveResult == smt::CheckResult::SATISFIABLE && negatedResult == smt::CheckResult::SATISFIABLE)
	{
		// everything fine.
	}
	else if (positiveResult == smt::CheckResult::UNKNOWN || negatedResult == smt::CheckResult::UNKNOWN)
	{
		// can't do anything.
	}
	else if (positiveResult == smt::CheckResult::UNSATISFIABLE && negatedResult == smt::CheckResult::UNSATISFIABLE)
		m_errorReporter.warning(_condition.location(), "Condition unreachable.");
	else
	{
		string value;
		if (positiveResult == smt::CheckResult::SATISFIABLE)
		{
			solAssert(negatedResult == smt::CheckResult::UNSATISFIABLE, "");
			value = "true";
		}
		else
		{
			solAssert(positiveResult == smt::CheckResult::UNSATISFIABLE, "");
			solAssert(negatedResult == smt::CheckResult::SATISFIABLE, "");
			value = "false";
		}
		m_errorReporter.warning(_condition.location(), boost::algorithm::replace_all_copy(_description, "$VALUE", value));
	}
}

pair<smt::CheckResult, vector<string>>
SMTChecker::checkSatisfiableAndGenerateModel(vector<smt::Expression> const& _expressionsToEvaluate)
{
	smt::CheckResult result;
	vector<string> values;
	try
	{
		tie(result, values) = m_interface->check(_expressionsToEvaluate);
	}
	catch (smt::SolverError const& _e)
	{
		string description("Error querying SMT solver");
		if (_e.comment())
			description += ": " + *_e.comment();
		m_errorReporter.warning(description);
		result = smt::CheckResult::ERROR;
	}

	for (string& value: values)
	{
		try
		{
			// Parse and re-format nicely
			value = formatNumberReadable(bigint(value));
		}
		catch (...) { }
	}

	return make_pair(result, values);
}

smt::CheckResult SMTChecker::checkSatisfiable()
{
	return checkSatisfiableAndGenerateModel({}).first;
}

void SMTChecker::initializeFunctionCallParameters(FunctionDefinition const& _function, vector<smt::Expression> const& _callArgs)
{
	auto const& funParams = _function.parameters();
	solAssert(funParams.size() == _callArgs.size(), "");
	for (unsigned i = 0; i < funParams.size(); ++i)
		if (createVariable(*funParams[i]))
		{
			m_interface->addAssertion(_callArgs[i] == newValue(*funParams[i]));
			if (funParams[i]->annotation().type->category() == Type::Category::Mapping)
				m_arrayAssignmentHappened = true;
		}

	for (auto const& variable: _function.localVariables())
		if (createVariable(*variable))
		{
			newValue(*variable);
			setZeroValue(*variable);
		}

	if (_function.returnParameterList())
		for (auto const& retParam: _function.returnParameters())
			if (createVariable(*retParam))
			{
				newValue(*retParam);
				setZeroValue(*retParam);
			}
}

void SMTChecker::initializeLocalVariables(FunctionDefinition const& _function)
{
	for (auto const& variable: _function.localVariables())
		if (createVariable(*variable))
			setZeroValue(*variable);

	for (auto const& param: _function.parameters())
		if (createVariable(*param))
			setUnknownValue(*param);

	if (_function.returnParameterList())
		for (auto const& retParam: _function.returnParameters())
			if (createVariable(*retParam))
				setZeroValue(*retParam);
}

void SMTChecker::removeLocalVariables()
{
	for (auto it = m_variables.begin(); it != m_variables.end(); )
	{
		if (it->first->isLocalVariable())
			it = m_variables.erase(it);
		else
			++it;
	}
}

void SMTChecker::resetStateVariables()
{
	for (auto const& variable: m_variables)
	{
		if (variable.first->isStateVariable())
		{
			newValue(*variable.first);
			setUnknownValue(*variable.first);
		}
	}
}

void SMTChecker::resetVariables(vector<VariableDeclaration const*> _variables)
{
	for (auto const* decl: _variables)
	{
		newValue(*decl);
		setUnknownValue(*decl);
	}
}

void SMTChecker::mergeVariables(vector<VariableDeclaration const*> const& _variables, smt::Expression const& _condition, VariableIndices const& _indicesEndTrue, VariableIndices const& _indicesEndFalse)
{
	set<VariableDeclaration const*> uniqueVars(_variables.begin(), _variables.end());
	for (auto const* decl: uniqueVars)
	{
		solAssert(_indicesEndTrue.count(decl) && _indicesEndFalse.count(decl), "");
		int trueIndex = _indicesEndTrue.at(decl);
		int falseIndex = _indicesEndFalse.at(decl);
		solAssert(trueIndex != falseIndex, "");
		m_interface->addAssertion(newValue(*decl) == smt::Expression::ite(
			_condition,
			valueAtIndex(*decl, trueIndex),
			valueAtIndex(*decl, falseIndex))
		);
	}
}

bool SMTChecker::createVariable(VariableDeclaration const& _varDecl)
{
	// This might be the case for multiple calls to the same function.
	if (knownVariable(_varDecl))
		return true;
	auto const& type = _varDecl.type();
	solAssert(m_variables.count(&_varDecl) == 0, "");
	auto result = newSymbolicVariable(*type, _varDecl.name() + "_" + to_string(_varDecl.id()), *m_interface);
	m_variables.emplace(&_varDecl, result.second);
	if (result.first)
	{
		m_errorReporter.warning(
			_varDecl.location(),
			"Assertion checker does not yet support the type of this variable."
		);
		return false;
	}
	return true;
}

bool SMTChecker::knownVariable(VariableDeclaration const& _decl)
{
	return m_variables.count(&_decl);
}

smt::Expression SMTChecker::currentValue(VariableDeclaration const& _decl)
{
	solAssert(knownVariable(_decl), "");
	return m_variables.at(&_decl)->currentValue();
}

smt::Expression SMTChecker::valueAtIndex(VariableDeclaration const& _decl, int _index)
{
	solAssert(knownVariable(_decl), "");
	return m_variables.at(&_decl)->valueAtIndex(_index);
}

smt::Expression SMTChecker::newValue(VariableDeclaration const& _decl)
{
	solAssert(knownVariable(_decl), "");
	return m_variables.at(&_decl)->increaseIndex();
}

void SMTChecker::setZeroValue(VariableDeclaration const& _decl)
{
	solAssert(knownVariable(_decl), "");
	setZeroValue(*m_variables.at(&_decl));
}

void SMTChecker::setZeroValue(SymbolicVariable& _variable)
{
	smt::setSymbolicZeroValue(_variable, *m_interface);
}

void SMTChecker::setUnknownValue(VariableDeclaration const& _decl)
{
	solAssert(knownVariable(_decl), "");
	setUnknownValue(*m_variables.at(&_decl));
}

void SMTChecker::setUnknownValue(SymbolicVariable& _variable)
{
	smt::setSymbolicUnknownValue(_variable, *m_interface);
}

smt::Expression SMTChecker::expr(Expression const& _e)
{
	if (!knownExpr(_e))
	{
		m_errorReporter.warning(_e.location(), "Internal error: Expression undefined for SMT solver." );
		createExpr(_e);
	}
	return m_expressions.at(&_e)->currentValue();
}

bool SMTChecker::knownExpr(Expression const& _e) const
{
	return m_expressions.count(&_e);
}

bool SMTChecker::knownGlobalSymbol(string const& _var) const
{
	return m_globalContext.count(_var);
}

void SMTChecker::createExpr(Expression const& _e)
{
	solAssert(_e.annotation().type, "");
	if (knownExpr(_e))
		m_expressions.at(&_e)->increaseIndex();
	else
	{
		auto result = newSymbolicVariable(*_e.annotation().type, "expr_" + to_string(_e.id()), *m_interface);
		m_expressions.emplace(&_e, result.second);
		if (result.first)
			m_errorReporter.warning(
				_e.location(),
				"Assertion checker does not yet implement this type."
			);
	}
}

void SMTChecker::defineExpr(Expression const& _e, smt::Expression _value)
{
	createExpr(_e);
	solAssert(isSupportedType(*_e.annotation().type), "Equality operator applied to type that is not fully supported");
	m_interface->addAssertion(expr(_e) == _value);
}

void SMTChecker::popPathCondition()
{
	solAssert(m_pathConditions.size() > 0, "Cannot pop path condition, empty.");
	m_pathConditions.pop_back();
}

void SMTChecker::pushPathCondition(smt::Expression const& _e)
{
	m_pathConditions.push_back(currentPathConditions() && _e);
}

smt::Expression SMTChecker::currentPathConditions()
{
	if (m_pathConditions.empty())
		return smt::Expression(true);
	return m_pathConditions.back();
}

void SMTChecker::addPathConjoinedExpression(smt::Expression const& _e)
{
	m_interface->addAssertion(currentPathConditions() && _e);
}

void SMTChecker::addPathImpliedExpression(smt::Expression const& _e)
{
	m_interface->addAssertion(smt::Expression::implies(currentPathConditions(), _e));
}

bool SMTChecker::isRootFunction()
{
	return m_functionPath.size() == 1;
}

bool SMTChecker::visitedFunction(FunctionDefinition const* _funDef)
{
	return contains(m_functionPath, _funDef);
}

SMTChecker::VariableIndices SMTChecker::copyVariableIndices()
{
	VariableIndices indices;
	for (auto const& var: m_variables)
		indices.emplace(var.first, var.second->index());
	return indices;
}

void SMTChecker::resetVariableIndices(VariableIndices const& _indices)
{
	for (auto const& var: _indices)
		m_variables.at(var.first)->index() = var.second;
}
