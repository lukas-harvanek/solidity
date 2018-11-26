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
/**
 * @author Christian <c@ethdev.com>
 * @date 2014
 * Formatting functions for errors referencing positions and locations in the source.
 */

#include <liblangutil/SourceReferenceFormatterHuman.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/Exceptions.h>
#include <libdevcore/termcolor.h>
#include <cmath>
#include <iomanip>

using namespace std;
using namespace dev;
using namespace langutil;

namespace
{
inline ostream& frameColor(ostream& os) { return os << termcolor::bold << termcolor::cyan; };
inline ostream& messageColor(ostream& os) { return os << termcolor::bold << termcolor::white; };
inline ostream& errorColor(ostream& os) { return os << termcolor::bold << termcolor::red; };
inline ostream& highlightColor(ostream& os) { return os << termcolor::yellow; };
inline ostream& diagColor(ostream& os) { return os << termcolor::bold << termcolor::yellow; };
}

void SourceReferenceFormatterHuman::printSourceLocation(SourceReference const& _ref)
{
	if (_ref.position.line < 0)
		return; // Nothing we can print here

	int const leftpad = static_cast<int>(log10(max(_ref.position.line, 1))) + 1;

	// line 0: source name
	m_stream << string(leftpad, ' ') << frameColor << "--> " << termcolor::reset
			 << _ref.sourceName << ":" << (_ref.position.line + 1) << ":" << (_ref.position.column + 1) << ": "
			 << '\n';

	if (!_ref.multiline)
	{
		int const locationLength = _ref.endColumn - _ref.startColumn;

		// line 1:
		m_stream << string(leftpad, ' ') << frameColor << " |" << termcolor::reset << '\n';

		// line 2:
		m_stream << frameColor << (_ref.position.line + 1) << " | " << termcolor::reset
				 << _ref.text.substr(0, _ref.startColumn)
				 << highlightColor << _ref.text.substr(_ref.startColumn, locationLength) << termcolor::reset
				 << _ref.text.substr(_ref.endColumn) << '\n';

		// line 3:
		m_stream << string(leftpad, ' ') << frameColor << " | " << termcolor::reset;
		for_each(
			_ref.text.cbegin(),
			_ref.text.cbegin() + _ref.startColumn,
			[this](char ch) { m_stream << (ch == '\t' ? '\t' : ' '); }
		);
		m_stream << diagColor << string(locationLength, '^') << termcolor::reset << '\n';
	}
	else
	{
		// line 1:
		m_stream << string(leftpad, ' ') << frameColor << " |" << termcolor::reset << '\n';

		// line 2:
		m_stream << frameColor << (_ref.position.line + 1) << " | " << termcolor::reset
				 << _ref.text.substr(0, _ref.startColumn)
				 << highlightColor << _ref.text.substr(_ref.startColumn) << termcolor::reset << '\n';

		// line 3:
		m_stream << string(leftpad, ' ') << frameColor << " | " << termcolor::reset
				 << string(_ref.startColumn, ' ')
				 << diagColor << "^ (Relevant source part starts here and spans across multiple lines)."
				 << termcolor::reset << '\n';
	}

	m_stream << '\n';
}

void SourceReferenceFormatterHuman::printExceptionInformation(dev::Exception const& _error, std::string const& _category)
{
	auto const ref = SourceReferenceExtractor::extract(_error, _category);

	// exception header line
	m_stream << errorColor << ref.category;
	if (string const* description = boost::get_error_info<errinfo_comment>(_error))
		m_stream << messageColor << ": " << *description;
	m_stream << termcolor::reset << '\n';

	if (ref.primary.position.line < 0 || ref.primary.sourceName.empty())
		m_stream << '\n';

	printSourceLocation(ref.primary);

	for (auto const& secondary : ref.secondary)
		printSourceLocation(secondary);
}
