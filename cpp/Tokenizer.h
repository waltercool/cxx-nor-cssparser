// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cssparser
{
namespace detail
{

/*!
 * A single CSS token, following the token types defined by the CSS Syntax
 * specification, in a simplified form.
 */
struct Token {
    enum class Type : uint8_t {
        Delim, //< A single character like `{`, `;`, `#`, `!`, `&`, ... Multi-character operators like `~=`, `>>>` are combined into one token.
        Ident, //< An identifier, including custom idents like `--foo` and `-moz-bar`. Stored in \a text.
        AtKeyword, //< An at-rule keyword like `@media`. The `@` is not included in \a text.
        Hash, //< A hash like `#foo`, as used by ID selectors and hex colors. The `#` is not included in \a text.
        QuotedString, //< A quoted string, with escape sequences decoded and the quotes removed. Stored in \a text.
        Number, //< A numeric value, stored in \a number.
        Percentage, //< A numeric value followed by `%`, stored in \a number.
        Dimension, //< A numeric value followed by a unit, stored in \a number and \a unit.
        Url, //< A url() token, with the URL stored in \a url.
        Function, //< An identifier immediately followed by `(`, stored in \a text. The matching `)` is consumed by the parser.
        End, //< End of input.
    };

    Type type = Type::End;
    std::string text; //< Payload for Delim, Ident, AtKeyword, Hash, QuotedString and Function tokens.
    double number = 0.0; //< Value of Number, Percentage and Dimension tokens.
    std::string unit; //< Unit of Dimension tokens, as written.
    std::string url; //< URL of Url tokens.
    bool wsBefore = false; //< Whether whitespace or a comment preceded this token.
    uint32_t line = 1; //< Line this token starts on.
    uint32_t column = 1; //< Column this token starts on.

    bool isDelim(char c) const;
    bool isIdent(const std::string &name) const;
    std::string toString() const;
};

/*!
 * Tokenize the whole of \a input into a flat list of tokens.
 *
 * Whitespace and comments are dropped, but each token records whether it was
 * preceded by whitespace in \a wsBefore, which the parser uses to detect
 * descendant combinators and space separated lists.
 */
std::vector<Token> tokenize(const std::string &input);

}
}
