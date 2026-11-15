// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "Tokenizer.h"

#include <cctype>
#include <charconv>

namespace cssparser
{
namespace detail
{

bool Token::isDelim(char c) const
{
    return type == Type::Delim && text.size() == 1 && text[0] == c;
}

bool Token::isIdent(const std::string &name) const
{
    return type == Type::Ident && text == name;
}

std::string Token::toString() const
{
    switch (type) {
    case Type::Delim:
        return std::string{text};
    case Type::Ident:
        return text;
    case Type::AtKeyword:
        return "@" + text;
    case Type::Hash:
        return "#" + text;
    case Type::QuotedString:
        return "\"" + text + "\"";
    case Type::Number:
        return std::to_string(number);
    case Type::Percentage:
        return std::to_string(number) + "%";
    case Type::Dimension:
        return std::to_string(number) + unit;
    case Type::Url:
        return "url(" + url + ")";
    case Type::Function:
        return text + "(";
    case Type::End:
        return "<end of input>";
    }

    return "<unknown token>";
}

namespace
{

bool isIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '-';
}

bool isHexDigit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

/*!
 * A cursor over the source input, tracking line/column for error reporting.
 */
class Cursor
{
public:
    explicit Cursor(const std::string &input)
        : m_input(input)
    {
    }

    bool atEnd() const
    {
        return m_pos >= m_input.size();
    }

    char peek(size_t lookahead = 0) const
    {
        if (m_pos + lookahead >= m_input.size()) {
            return '\0';
        }
        return m_input[m_pos + lookahead];
    }

    void advance(size_t count = 1)
    {
        for (size_t i = 0; i < count && m_pos < m_input.size(); ++i) {
            if (m_input[m_pos] == '\n') {
                m_line++;
                m_column = 1;
            } else {
                m_column++;
            }
            m_pos++;
        }
    }

    size_t position() const
    {
        return m_pos;
    }

    uint32_t line() const
    {
        return m_line;
    }

    uint32_t column() const
    {
        return m_column;
    }

    // Consume an escape sequence at the current position, appending the decoded
    // character(s) to \a out. Returns false if there is no escape sequence.
    bool consumeEscape(std::string &out)
    {
        if (peek() != '\\') {
            return false;
        }

        advance();
        char c = peek();
        if (atEnd()) {
            out += '\\';
            return true;
        }

        if (isHexDigit(c)) {
            uint32_t value = 0;
            size_t digits = 0;
            while (digits < 6 && isHexDigit(peek())) {
                char digit = peek();
                uint32_t digitValue = digit <= '9' ? digit - '0' : (digit >= 'a' ? digit - 'a' + 10 : digit - 'A' + 10);
                value = value * 16 + digitValue;
                advance();
                digits++;
            }
            // A whitespace character after the hex sequence is part of the escape.
            if (std::isspace(static_cast<unsigned char>(peek()))) {
                advance();
            }
            appendUtf8(out, value);
            return true;
        }

        if (c == '\n') {
            // Line continuation: escapes may not contain newlines in CSS, treat
            // as literal replacement character.
            out += '\xEF';
            out += '\xBF';
            out += '\xBD';
            advance();
            return true;
        }

        out += c;
        advance();
        return true;
    }

    // Consume an identifier at the current position. Returns empty string if
    // the input does not start an identifier.
    std::string consumeIdent()
    {
        if (!isIdentStart(peek()) && !(peek() == '-' && (isIdentStart(peek(1)) || peek(1) == '-'))) {
            return {};
        }

        std::string result;
        while (!atEnd()) {
            if (peek() == '\\') {
                consumeEscape(result);
            } else if (isIdentChar(peek())) {
                result += peek();
                advance();
            } else {
                break;
            }
        }
        return result;
    }

    // Consume the name part of a hash token (`#name`). Unlike an identifier,
    // a hash name may start with a digit, so this accepts any ident code point
    // directly. Returns empty string when what follows the `#` is not an ident
    // code point, in which case the `#` is a plain delimiter.
    std::string consumeHashName()
    {
        if (!isIdentChar(peek())) {
            return {};
        }

        std::string result;
        while (!atEnd()) {
            if (peek() == '\\') {
                consumeEscape(result);
            } else if (isIdentChar(peek())) {
                result += peek();
                advance();
            } else {
                break;
            }
        }
        return result;
    }

    // Consume a number at the current position. Returns false if no number starts here.
    bool consumeNumber(double &value)
    {
        size_t start = m_pos;
        size_t i = m_pos;

        if (m_input[i] == '+' || m_input[i] == '-') {
            i++;
        }

        size_t digitStart = i;
        while (i < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[i]))) {
            i++;
        }
        if (i == digitStart) {
            return false;
        }

        if (i + 1 < m_input.size() && m_input[i] == '.' && std::isdigit(static_cast<unsigned char>(m_input[i + 1]))) {
            i++; // the dot
            while (i < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[i]))) {
                i++;
            }
        }

        // Scientific notation, only when there are digits after e/E(+/-).
        if (i < m_input.size() && (m_input[i] == 'e' || m_input[i] == 'E')) {
            size_t exp = i;
            i++;
            if (i < m_input.size() && (m_input[i] == '+' || m_input[i] == '-')) {
                i++;
            }
            size_t expDigits = i;
            while (i < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[i]))) {
                i++;
            }
            if (i == expDigits) {
                i = exp; // Not an exponent, `e` is the start of a unit instead.
            }
        }

        std::string numberText = m_input.substr(start, i - start);
        double parsed = 0.0;
        auto [ptr, ec] = std::from_chars(numberText.data(), numberText.data() + numberText.size(), parsed);
        if (ec != std::errc{}) {
            return false;
        }

        value = parsed;
        m_pos = i;
        // Recalculate line/column the slow but simple way.
        recalculatePositions(start, i);
        return true;
    }

    // Consume a quoted string at the current position. Returns false if the
    // input does not start a quoted string.
    bool consumeQuotedString(std::string &out)
    {
        char quote = peek();
        if (quote != '"' && quote != '\'') {
            return false;
        }

        advance();
        out.clear();
        while (!atEnd()) {
            if (peek() == quote) {
                advance();
                return true;
            }
            if (peek() == '\\') {
                consumeEscape(out);
            } else if (peek() == '\n') {
                // Unterminated string
                return false;
            } else {
                out += peek();
                advance();
            }
        }
        return true; // Unterminated string ends at end of input.
    }

    std::string_view input() const
    {
        return m_input;
    }

private:
    void recalculatePositions(size_t from, size_t to)
    {
        // m_line/m_column are correct at m_pos == from; walk forward one char at a time.
        for (size_t i = from; i < to && i < m_input.size(); ++i) {
            if (m_input[i] == '\n') {
                m_line++;
                m_column = 1;
            } else {
                m_column++;
            }
        }
        m_pos = to;
    }

    static void appendUtf8(std::string &out, uint32_t codePoint)
    {
        if (codePoint == 0 || codePoint > 0x10FFFF) {
            codePoint = 0xFFFD; // Replacement character.
        }
        if (codePoint <= 0x7F) {
            out += static_cast<char>(codePoint);
        } else if (codePoint <= 0x7FF) {
            out += static_cast<char>(0xC0 | (codePoint >> 6));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else if (codePoint <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (codePoint >> 12));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (codePoint >> 18));
            out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codePoint & 0x3F));
        }
    }

    const std::string &m_input;
    size_t m_pos = 0;
    uint32_t m_line = 1;
    uint32_t m_column = 1;
};

// Skip whitespace and comments, returning whether anything was skipped.
bool skipWhitespace(const std::string &input, size_t &pos)
{
    bool skipped = false;
    while (pos < input.size()) {
        char c = input[pos];
        if (std::isspace(static_cast<unsigned char>(c))) {
            pos++;
            skipped = true;
        } else if (c == '/' && pos + 1 < input.size() && input[pos + 1] == '*') {
            size_t end = input.find("*/", pos + 2);
            pos = (end == std::string::npos) ? input.size() : end + 2;
            skipped = true;
        } else {
            break;
        }
    }
    return skipped;
}

} // namespace

std::vector<Token> tokenize(const std::string &input)
{
    std::vector<Token> tokens;
    Cursor cursor(input);
    bool pendingWhitespace = false;

    auto beginToken = [&]() {
        Token token;
        token.wsBefore = pendingWhitespace;
        token.line = cursor.line();
        token.column = cursor.column();
        pendingWhitespace = false;
        return token;
    };

    while (true) {
        // Skip whitespace and comments manually so we can track positions and
        // the wsBefore flag.
        {
            size_t pos = cursor.position();
            if (skipWhitespace(input, pos)) {
                pendingWhitespace = true;
            }
            // Advance cursor one character at a time to keep line/col correct.
            while (cursor.position() < pos) {
                cursor.advance();
            }
        }

        if (cursor.atEnd()) {
            Token token = beginToken();
            token.type = Token::Type::End;
            tokens.push_back(std::move(token));
            break;
        }

        char c = cursor.peek();

        // Two- and three-character operators that must not be split into
        // individual delimiters.
        auto startsWith = [&](const char *str) {
            return input.compare(cursor.position(), std::char_traits<char>::length(str), str) == 0;
        };

        if (startsWith(">>>")) {
            Token token = beginToken();
            token.type = Token::Type::Delim;
            token.text = ">>>";
            cursor.advance(3);
            tokens.push_back(std::move(token));
            continue;
        }

        if (startsWith("~=") || startsWith("$=") || startsWith("*=") || startsWith("|=") || startsWith("->")) {
            Token token = beginToken();
            token.type = Token::Type::Delim;
            token.text = input.substr(cursor.position(), 2);
            cursor.advance(2);
            tokens.push_back(std::move(token));
            continue;
        }

        // Numbers, percentages and dimensions.
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(cursor.peek(1))))) {
            double value = 0.0;
            if (!cursor.consumeNumber(value)) {
                // Should not happen, consume as delimiter to guarantee progress.
                Token token = beginToken();
                token.type = Token::Type::Delim;
                token.text = std::string(1, c);
                cursor.advance();
                tokens.push_back(std::move(token));
                continue;
            }

            if (cursor.peek() == '%') {
                Token token = beginToken();
                token.type = Token::Type::Percentage;
                token.number = value;
                cursor.advance();
                tokens.push_back(std::move(token));
                continue;
            }

            // If an identifier immediately follows the number, this is a
            // dimension. Note that `5e-3` was already handled by consumeNumber,
            // and that consumeIdent() consumes nothing when what follows is not
            // an identifier.
            std::string unit = cursor.consumeIdent();
            if (!unit.empty()) {
                Token token = beginToken();
                token.type = Token::Type::Dimension;
                token.number = value;
                token.unit = unit;
                tokens.push_back(std::move(token));
                continue;
            }

            Token token = beginToken();
            token.type = Token::Type::Number;
            token.number = value;
            tokens.push_back(std::move(token));
            continue;
        }

        // Url token.
        if ((c == 'u' || c == 'U') && startsWith("url(")) {
            // `url(` with no space between name and parenthesis is a url token.
            std::string urlValue;
            cursor.advance(4);
            // Skip leading whitespace inside the url.
            while (!cursor.atEnd() && std::isspace(static_cast<unsigned char>(cursor.peek()))) {
                cursor.advance();
            }

            std::string quoted;
            if (cursor.consumeQuotedString(quoted)) {
                urlValue = quoted;
                // Skip trailing whitespace and expect `)`.
                while (!cursor.atEnd() && std::isspace(static_cast<unsigned char>(cursor.peek()))) {
                    cursor.advance();
                }
                if (cursor.peek() == ')') {
                    cursor.advance();
                }
            } else {
                size_t start = cursor.position();
                while (!cursor.atEnd() && cursor.peek() != ')') {
                    if (cursor.peek() == '\\') {
                        std::string escape;
                        cursor.consumeEscape(escape);
                    } else {
                        cursor.advance();
                    }
                }
                urlValue = input.substr(start, cursor.position() - start);
                // Trim trailing whitespace from unquoted URLs.
                while (!urlValue.empty() && std::isspace(static_cast<unsigned char>(urlValue.back()))) {
                    urlValue.pop_back();
                }
                if (cursor.peek() == ')') {
                    cursor.advance();
                }
            }

            Token token = beginToken();
            token.type = Token::Type::Url;
            token.url = urlValue;
            tokens.push_back(std::move(token));
            continue;
        }

        // At-keyword.
        if (c == '@' && isIdentStart(cursor.peek(1))) {
            cursor.advance();
            std::string name = cursor.consumeIdent();
            Token token = beginToken();
            token.type = Token::Type::AtKeyword;
            token.text = name;
            tokens.push_back(std::move(token));
            continue;
        }

        // Hash.
        if (c == '#') {
            cursor.advance();
            std::string name = cursor.consumeHashName();
            Token token = beginToken();
            if (!name.empty()) {
                token.type = Token::Type::Hash;
                token.text = name;
            } else {
                token.type = Token::Type::Delim;
                token.text = "#";
            }
            tokens.push_back(std::move(token));
            continue;
        }

        // Quoted string.
        if (c == '"' || c == '\'') {
            std::string value;
            cursor.consumeQuotedString(value);
            Token token = beginToken();
            token.type = Token::Type::QuotedString;
            token.text = value;
            tokens.push_back(std::move(token));
            continue;
        }

        // Identifier or function.
        if (isIdentStart(c) || (c == '-' && (isIdentStart(cursor.peek(1)) || cursor.peek(1) == '-'))) {
            std::string name = cursor.consumeIdent();
            if (!name.empty() && cursor.peek() == '(') {
                cursor.advance();
                Token token = beginToken();
                token.type = Token::Type::Function;
                token.text = name;
                tokens.push_back(std::move(token));
            } else {
                Token token = beginToken();
                token.type = Token::Type::Ident;
                token.text = name;
                tokens.push_back(std::move(token));
            }
            continue;
        }

        // Any other character is a single-character delimiter.
        Token token = beginToken();
        token.type = Token::Type::Delim;
        token.text = std::string(1, c);
        cursor.advance();
        tokens.push_back(std::move(token));
    }

    return tokens;
}

}
}
