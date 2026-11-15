// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "Parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>

namespace cssparser
{
namespace detail
{

std::string ParseError::toString() const
{
    const std::string prefix = std::format("In file \"{}\" at line {} column {}: ", file, line, column);

    std::string text;
    switch (kind) {
    case Kind::Unspecified:
        text = "Unspecified error";
        break;
    case Kind::Unimplemented:
        text = "Feature is not yet implemented";
        break;
    case Kind::UnexpectedEndOfInput:
        text = "Unexpected end of input";
        break;
    case Kind::Unknown:
        text = std::format("Unknown error: {}", message);
        break;
    case Kind::UnknownProperty:
        text = std::format("Unknown Property: {}", message);
        break;
    case Kind::UnexpectedToken:
        text = std::format("Unexpected Token: {}", message);
        break;
    case Kind::InvalidSelectors:
        text = std::format("Invalid Selectors: {}", message);
        break;
    case Kind::InvalidPropertySyntax:
        text = std::format("Invalid property syntax: {}", message);
        break;
    case Kind::InvalidPropertyValue:
        text = std::format("Invalid property value: {}", message);
        break;
    case Kind::UnknownFunction:
        text = std::format("Unknown function: {}", message);
        break;
    case Kind::InvalidPropertyDefinition:
        text = std::format("Invalid property definition: {}", message);
        break;
    case Kind::PropertyValueDoesNotMatchSyntax:
        text = std::format("Property value does not match syntax: {}", message);
        break;
    case Kind::UnsupportedAtRule:
        text = std::format("Unsupported @-rule: {}", message);
        break;
    case Kind::InvalidAtRule:
        text = std::format("Invalid @-rule: {}", message);
        break;
    case Kind::InvalidQualifiedRule:
        text = "Invalid qualified rule";
        break;
    case Kind::FileError:
        text = std::format("IO Error: {}", message);
        break;
    case Kind::StyleSheetParseError:
        text = std::format("Stylesheet failed to parse: {}", message);
        break;
    }

    return prefix + text;
}

// --- Parser -------------------------------------------------------------------

const Token Parser::s_endToken = {};

Parser::Parser(std::vector<Token> tokens, std::string sourceName)
    : m_tokens(std::move(tokens))
    , m_sourceName(std::move(sourceName))
{
}

const Token &Parser::peek(size_t lookahead) const
{
    if (m_pos + lookahead >= m_tokens.size()) {
        return s_endToken;
    }
    return m_tokens[m_pos + lookahead];
}

const Token &Parser::next()
{
    if (m_pos >= m_tokens.size()) {
        return s_endToken;
    }
    return m_tokens[m_pos++];
}

bool Parser::atEnd() const
{
    return peek().type == Token::Type::End;
}

size_t Parser::mark() const
{
    return m_pos;
}

void Parser::reset(size_t mark)
{
    m_pos = std::min(mark, m_tokens.size());
}

bool Parser::check(Token::Type type) const
{
    return peek().type == type;
}

bool Parser::checkDelim(char c) const
{
    return peek().isDelim(c);
}

bool Parser::checkIdent(const std::string &name) const
{
    return peek().isIdent(name);
}

bool Parser::consumeDelim(char c)
{
    if (checkDelim(c)) {
        next();
        return true;
    }
    return false;
}

std::expected<std::string, ParseError> Parser::expectIdent()
{
    if (atEnd()) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
    }
    if (peek().type != Token::Type::Ident) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected an identifier, got {}", peek().toString())));
    }
    return {next().text};
}

std::expected<double, ParseError> Parser::expectNumber()
{
    if (atEnd()) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
    }
    if (peek().type != Token::Type::Number) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected a number, got {}", peek().toString())));
    }
    return {next().number};
}

std::expected<int, ParseError> Parser::expectInteger()
{
    auto number = expectNumber();
    if (!number.has_value()) {
        return std::unexpected(number.error());
    }
    if (std::trunc(*number) != *number) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected an integer, got {}", *number)));
    }
    return {static_cast<int>(*number)};
}

std::expected<std::string, ParseError> Parser::expectString()
{
    if (atEnd()) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
    }
    if (peek().type != Token::Type::QuotedString) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected a string, got {}", peek().toString())));
    }
    return {next().text};
}

std::expected<std::string, ParseError> Parser::expectUrlOrString()
{
    if (atEnd()) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
    }

    if (peek().type == Token::Type::Url) {
        return {next().url};
    }
    if (peek().type == Token::Type::QuotedString) {
        return {next().text};
    }
    if (peek().type == Token::Type::Function && peek().text == "url") {
        // url("...") or url(...) call form: parse the contents as a single value.
        auto result = parseFunctionCall([](Parser &p) -> std::expected<std::string, ParseError> {
            if (p.atEnd()) {
                return std::unexpected(p.makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
            }
            if (p.peek().type == Token::Type::QuotedString) {
                return {p.next().text};
            }
            if (p.peek().type == Token::Type::Url) {
                return {p.next().url};
            }
            // Unquoted contents up to `)` are taken literally.
            std::string text;
            while (!p.atEnd() && !p.checkDelim(')')) {
                text += p.next().toString();
            }
            return {text};
        });
        return result;
    }

    return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected a url or a string, got {}", peek().toString())));
}

std::expected<void, ParseError> Parser::expectComma()
{
    if (atEnd()) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input"));
    }
    if (!consumeDelim(',')) {
        return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, std::format("Expected a comma, got {}", peek().toString())));
    }
    return {};
}

void Parser::skipBlock()
{
    if (!consumeDelim('{')) {
        return;
    }

    int depth = 1;
    while (!atEnd()) {
        if (checkDelim('{')) {
            depth++;
        } else if (checkDelim('}')) {
            depth--;
            if (depth == 0) {
                next();
                return;
            }
        }
        next();
    }
}

void Parser::skipUntilDelim(char c)
{
    int depth = 0;
    while (!atEnd()) {
        if (depth == 0 && consumeDelim(c)) {
            return;
        }
        if (checkDelim('{') || checkDelim('(') || check(Token::Type::Function)) {
            depth++;
        } else if (checkDelim('}') || checkDelim(')')) {
            if (depth > 0) {
                depth--;
            } else if (checkDelim('}')) {
                // Reached the end of the enclosing block; stop before it.
                return;
            }
        }
        next();
    }
}

bool Parser::seesBlockStart() const
{
    int depth = 0;
    for (size_t i = m_pos; i < m_tokens.size(); ++i) {
        const Token &token = m_tokens[i];
        if (token.type == Token::Type::End) {
            return false;
        }
        if (depth == 0) {
            if (token.isDelim('{')) {
                return true;
            }
            if (token.isDelim(';') || token.isDelim('}')) {
                return false;
            }
        }
        if (token.type == Token::Type::Function || token.isDelim('(')) {
            depth++;
        } else if (token.isDelim(')')) {
            depth = std::max(0, depth - 1);
        }
    }
    return false;
}

ParseError Parser::makeError(ParseError::Kind kind, const std::string &message) const
{
    ParseError error;
    error.kind = kind;
    error.message = message;
    error.file = m_sourceName;
    if (!atEnd()) {
        error.line = peek().line;
        error.column = peek().column;
    } else if (!m_tokens.empty()) {
        error.line = m_tokens.back().line;
        error.column = m_tokens.back().column;
    }
    return error;
}

ParseError Parser::ownedError(const ParseError &other) const
{
    ParseError error = other;
    if (error.file.empty()) {
        error.file = m_sourceName;
    }
    return error;
}

const std::string &Parser::sourceName() const
{
    return m_sourceName;
}

// --- Property syntax -------------------------------------------------------------
//
// Implements an extended version of the CSS custom property syntax, according to
// the following EBNF:
//
//   custom_ident_start ::= [A-Z] | [a-z] | "_" | (a range of unicode symbols)
//   custom_ident       ::= custom_ident_start | [0-9] | "-"
//   keyword            ::= custom_ident_start custom_ident*
//   data_type          ::= "<" data_type_name ">"
//   space_separated    ::= data_type "+"
//   comma_separated    ::= data_type "#"
//   repeats            ::= data_type "{" [0-9]+ "," [0-9]+ "}"
//   component          ::= data_type | keyword | space_separated | comma_separated | repeats | ","
//   group              ::= component | "(" expression ")"
//   alternatives       ::= group ("|" group)*
//   expression         ::= alternatives+

enum class DataType : uint8_t {
    Length,
    Number,
    Percentage,
    LengthPercentage,
    String,
    Color,
    Url,
    Integer,
    Angle,
    Time,
    Resolution,
    TransformFunction,
    CustomIdent,
};

std::string_view dataTypeName(DataType type);

struct ComponentDataType {
    DataType type;
};
struct ComponentKeyword {
    std::string name;
};
struct ComponentSpaceSeparatedList {
    DataType type;
};
struct ComponentCommaSeparatedList {
    DataType type;
};
struct ComponentRepeat {
    DataType type;
    size_t minimum;
    size_t maximum;
};
struct ComponentComma { };

using SyntaxComponent =
    std::variant<ComponentComma, ComponentDataType, ComponentKeyword, ComponentSpaceSeparatedList, ComponentCommaSeparatedList, ComponentRepeat>;

struct SyntaxGroup {
    std::variant<SyntaxComponent, std::vector<struct SyntaxAlternatives>> data;
};

struct SyntaxAlternatives {
    std::variant<SyntaxComponent, SyntaxGroup, std::vector<SyntaxGroup>> data;
};

struct ParsedPropertySyntax {
    enum class Kind : uint8_t {
        Empty,
        Universal,
        Expression
    } kind = Kind::Empty;
    std::vector<SyntaxAlternatives> expression;
};

namespace
{

bool isSyntaxIdentStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

bool isSyntaxIdentChar(char c)
{
    return isSyntaxIdentStart(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '-';
}

constexpr std::array<std::pair<std::string_view, DataType>, 13> supportedDataTypes = {
    {{"length-percentage", DataType::LengthPercentage},
     {"length", DataType::Length},
     {"number", DataType::Number},
     {"percentage", DataType::Percentage},
     {"string", DataType::String},
     {"color", DataType::Color},
     {"url", DataType::Url},
     {"integer", DataType::Integer},
     {"angle", DataType::Angle},
     {"time", DataType::Time},
     {"resolution", DataType::Resolution},
     {"transform-function", DataType::TransformFunction},
     {"custom-ident", DataType::CustomIdent}},
};

struct SyntaxParseContext {
    std::string_view input;
    size_t pos = 0;

    char peek() const
    {
        return pos < input.size() ? input[pos] : '\0';
    }

    void skipSpaces()
    {
        while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
            pos++;
        }
    }

    bool consumeChar(char c)
    {
        skipSpaces();
        if (peek() == c) {
            pos++;
            return true;
        }
        return false;
    }

    std::optional<DataType> matchDataTypeName()
    {
        for (const auto &[name, type] : supportedDataTypes) {
            if (input.compare(pos, name.size(), name) == 0) {
                pos += name.size();
                return {type};
            }
        }
        return std::nullopt;
    }

    std::expected<SyntaxComponent, ParseError> parseComponent()
    {
        skipSpaces();

        if (peek() == ',') {
            pos++;
            return {SyntaxComponent{ComponentComma{}}};
        }

        if (peek() == '<') {
            size_t start = pos;
            pos++;
            auto type = matchDataTypeName();
            if (!type.has_value()) {
                return std::unexpected(makeSyntaxError(start, "Input is not a valid data type"));
            }
            if (peek() != '>') {
                return std::unexpected(makeSyntaxError(start, "Expected '>' to close data type"));
            }
            pos++;

            // Suffixes must follow the '>' without a gap, like in the CSS grammar.
            if (peek() == '+') {
                pos++;
                return {SyntaxComponent{ComponentSpaceSeparatedList{*type}}};
            }
            if (peek() == '#') {
                pos++;
                return {SyntaxComponent{ComponentCommaSeparatedList{*type}}};
            }
            if (peek() == '{') {
                pos++;
                size_t minStart = pos;
                while (std::isdigit(static_cast<unsigned char>(peek()))) {
                    pos++;
                }
                if (pos == minStart || peek() != ',') {
                    return std::unexpected(makeSyntaxError(start, "Invalid repeat pattern"));
                }
                pos++;
                while (std::isspace(static_cast<unsigned char>(peek()))) {
                    pos++;
                }
                size_t maxStart = pos;
                while (std::isdigit(static_cast<unsigned char>(peek()))) {
                    pos++;
                }
                if (pos == maxStart || peek() != '}') {
                    return std::unexpected(makeSyntaxError(start, "Invalid repeat pattern"));
                }
                pos++;
                size_t minimum = std::stoul(std::string(input.substr(minStart, pos - minStart)));
                size_t maximum = std::stoul(std::string(input.substr(maxStart, pos - maxStart)));
                return {SyntaxComponent{ComponentRepeat{*type, minimum, maximum}}};
            }

            return {SyntaxComponent{ComponentDataType{*type}}};
        }

        // Keyword.
        size_t start = pos;
        if (!isSyntaxIdentStart(peek())) {
            return std::unexpected(makeSyntaxError(start, "Input is not a keyword"));
        }
        while (isSyntaxIdentChar(peek())) {
            pos++;
        }
        return {SyntaxComponent{ComponentKeyword{std::string(input.substr(start, pos - start))}}};
    }

    std::expected<SyntaxGroup, ParseError> parseGroup()
    {
        skipSpaces();

        if (peek() == '(') {
            size_t start = pos;
            pos++;
            auto expression = parseExpression(true);
            if (!expression.has_value()) {
                return std::unexpected(expression.error());
            }
            skipSpaces();
            if (peek() != ')') {
                return std::unexpected(makeSyntaxError(start, "Unbalanced parenthesis in property syntax"));
            }
            pos++;
            return {SyntaxGroup{*expression}};
        }

        auto component = parseComponent();
        if (!component.has_value()) {
            return std::unexpected(component.error());
        }
        return {SyntaxGroup{*component}};
    }

    // Parses one `|`-separated branch: a sequence of groups, consumed until the
    // next top-level `|` or a terminator (closing parenthesis / end of input).
    // This matters for branches like `(add | subtract | multiply) <color>` or
    // `set-alpha <number>`, where the branch continues after its first group.
    std::expected<bool, ParseError> parseBranch(std::vector<SyntaxAlternatives> &sequence)
    {
        while (true) {
            skipSpaces();
            if (peek() == '\0' || peek() == '|' || peek() == ')') {
                break;
            }
            auto group = parseGroup();
            if (!group.has_value()) {
                return std::unexpected(group.error());
            }
            sequence.push_back(SyntaxAlternatives{std::move(*group)});
        }
        return {!sequence.empty()};
    }

    std::expected<SyntaxAlternatives, ParseError> parseAlternatives()
    {
        // Collect the groups of this item up to the first `|` at this level.
        std::vector<SyntaxAlternatives> firstSequence;
        auto hasFirst = parseBranch(firstSequence);
        if (!hasFirst.has_value()) {
            return std::unexpected(hasFirst.error());
        }
        if (!*hasFirst) {
            return std::unexpected(makeSyntaxError(pos, "Input did not match an expression"));
        }

        skipSpaces();
        if (peek() != '|') {
            // No alternatives: unwrap single groups back into plain items so the
            // validator sees the same shape as before for simple syntaxes.
            if (firstSequence.size() == 1) {
                return {std::move(firstSequence.front())};
            }
            return {SyntaxAlternatives{SyntaxGroup{std::move(firstSequence)}}};
        }

        // One or more `|`-separated branches; each branch is a full sequence.
        std::vector<SyntaxGroup> branches;
        branches.push_back(SyntaxGroup{std::move(firstSequence)});
        while (peek() == '|') {
            pos++; // consume '|'
            std::vector<SyntaxAlternatives> branchSequence;
            if (!parseBranch(branchSequence).has_value()) {
                return std::unexpected(makeSyntaxError(pos, "Expected a component after '|'"));
            }
            if (branchSequence.empty()) {
                return std::unexpected(makeSyntaxError(pos, "Empty alternative after '|'"));
            }
            branches.push_back(SyntaxGroup{std::move(branchSequence)});
        }
        return {SyntaxAlternatives{branches}};
    }

    std::expected<std::vector<SyntaxAlternatives>, ParseError> parseExpression(bool nested = false)
    {
        std::vector<SyntaxAlternatives> result;
        while (true) {
            size_t checkpoint = pos;
            auto alternative = parseAlternatives();
            if (!alternative.has_value()) {
                pos = checkpoint;
                break;
            }
            result.push_back(std::move(*alternative));

            skipSpaces();
            if (nested && peek() == ')') {
                break;
            }
            if (nested && peek() == '|') {
                // Handled by parseAlternatives; should not get here.
                break;
            }
        }

        if (result.empty()) {
            return std::unexpected(makeSyntaxError(pos, "Input did not match an expression"));
        }
        return {result};
    }

    ParseError makeSyntaxError(size_t at, std::string_view what) const
    {
        ParseError error;
        error.kind = ParseError::Kind::InvalidPropertySyntax;
        error.message = std::format("Invalid property syntax at offset {}: {}", at, what);
        return error;
    }
};

std::string componentToString(const SyntaxComponent &component)
{
    return std::visit(
        [](const auto &entry) -> std::string {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, ComponentComma>) {
                return ",";
            } else if constexpr (std::is_same_v<T, ComponentDataType>) {
                return std::format("<{}>", dataTypeName(entry.type));
            } else if constexpr (std::is_same_v<T, ComponentKeyword>) {
                return entry.name;
            } else if constexpr (std::is_same_v<T, ComponentSpaceSeparatedList>) {
                return std::format("<{}>+", dataTypeName(entry.type));
            } else if constexpr (std::is_same_v<T, ComponentCommaSeparatedList>) {
                return std::format("<{}>#", dataTypeName(entry.type));
            } else if constexpr (std::is_same_v<T, ComponentRepeat>) {
                return std::format("<{}>{{{}, {}}}", dataTypeName(entry.type), entry.minimum, entry.maximum);
            } else {
                return "?";
            }
        },
        component);
}

std::string groupToString(const SyntaxGroup &group);

std::string alternativesToString(const SyntaxAlternatives &alternatives)
{
    return std::visit(
        [](const auto &entry) -> std::string {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, SyntaxComponent>) {
                return componentToString(entry);
            } else if constexpr (std::is_same_v<T, SyntaxGroup>) {
                return groupToString(entry);
            } else {
                std::string result;
                for (const auto &group : entry) {
                    if (!result.empty()) {
                        result += " | ";
                    }
                    result += groupToString(group);
                }
                return result;
            }
        },
        alternatives.data);
}

std::string groupToString(const SyntaxGroup &group)
{
    return std::visit(
        [](const auto &entry) -> std::string {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, SyntaxComponent>) {
                return componentToString(entry);
            } else {
                std::string result = "(";
                for (const auto &alternative : entry) {
                    if (result.size() > 1) {
                        result += " ";
                    }
                    result += alternativesToString(alternative);
                }
                return result + ")";
            }
        },
        group.data);
}

} // namespace

std::string_view dataTypeName(DataType type)
{
    switch (type) {
    case DataType::Length:
        return "length";
    case DataType::Number:
        return "number";
    case DataType::Percentage:
        return "percentage";
    case DataType::LengthPercentage:
        return "length-percentage";
    case DataType::String:
        return "string";
    case DataType::Color:
        return "color";
    case DataType::Url:
        return "url";
    case DataType::Integer:
        return "integer";
    case DataType::Angle:
        return "angle";
    case DataType::Time:
        return "time";
    case DataType::Resolution:
        return "resolution";
    case DataType::TransformFunction:
        return "transform-function";
    case DataType::CustomIdent:
        return "custom-ident";
    }
    return "unknown";
}

std::expected<ParsedPropertySyntax, ParseError> parseSyntaxText(const std::string &text, const ParseError &locationBase)
{
    SyntaxParseContext context{.input = text};

    context.skipSpaces();
    if (context.peek() == '*') {
        context.pos++;
        context.skipSpaces();
        if (context.pos != context.input.size()) {
            ParseError error = locationBase;
            error.kind = ParseError::Kind::InvalidPropertySyntax;
            error.message = "Unexpected characters after '*' in property syntax";
            return std::unexpected(error);
        }
        ParsedPropertySyntax syntax;
        syntax.kind = ParsedPropertySyntax::Kind::Universal;
        return {syntax};
    }

    auto expression = context.parseExpression(false);
    if (!expression.has_value()) {
        ParseError error = locationBase;
        error.kind = ParseError::Kind::InvalidPropertySyntax;
        error.message = std::format("Invalid property syntax \"{}\": {}", text, expression.error().message);
        return std::unexpected(error);
    }

    context.skipSpaces();
    if (context.pos != context.input.size()) {
        ParseError error = locationBase;
        error.kind = ParseError::Kind::InvalidPropertySyntax;
        error.message = std::format("Unexpected trailing characters in property syntax \"{}\"", text);
        return std::unexpected(error);
    }

    ParsedPropertySyntax syntax;
    syntax.kind = ParsedPropertySyntax::Kind::Expression;
    syntax.expression = std::move(*expression);
    return {syntax};
}

// --- Values ---------------------------------------------------------------------

namespace
{

const ParsedPropertySyntax &universalSyntax()
{
    static const ParsedPropertySyntax syntax{.kind = ParsedPropertySyntax::Kind::Universal};
    return syntax;
}

const ParsedPropertySyntax &syntaxFromText(const char *text)
{
    // Cache one parsed syntax per distinct text. The map is intentionally
    // leaked so parsed syntaxes stay valid for the process lifetime.
    static auto &cache = *new std::map<std::string, ParsedPropertySyntax>;

    auto it = cache.find(text);
    if (it != cache.end()) {
        return it->second;
    }

    auto result = parseSyntaxText(text, ParseError{});
    if (!result.has_value()) {
        throw std::runtime_error(result.error().toString());
    }
    return cache.emplace(text, std::move(*result)).first->second;
}

std::string asciiLower(std::string_view text)
{
    std::string result{text};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return result;
}

Value makeNumberValue(double value)
{
    Dimension dimension{Dimension::Unit::Number, static_cast<float>(value)};
    return Value{Value::Type::Dimension, dimension};
}

Value makeDimensionValue(Dimension::Unit unit, double value)
{
    Dimension dimension{unit, static_cast<float>(value)};
    return Value{Value::Type::Dimension, dimension};
}

Value makeStringValue(const std::string &text)
{
    return Value{Value::Type::String, text};
}

Value makeIntegerValue(int number)
{
    return Value{Value::Type::Integer, number};
}

Value makeUrlValue(const std::string &url)
{
    return Value{Value::Type::Url, url};
}

Value makeRgbaColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    Color::Color color{Color::Color::Type::Rgba, Color::RgbaData{r, g, b, a}};
    return Value{Value::Type::Color, color};
}

// A short display representation of a Value, used in validation error messages.
// This mirrors the structure of Value::toString() while staying compact.
std::string valueDisplay(const Value &value)
{
    switch (value.type()) {
    case Value::Type::Empty:
        return "Empty Value";
    case Value::Type::Dimension: {
        const auto &dimension = value.get<Dimension>();
        return std::format("Dimension({})", dimension.toString());
    }
    case Value::Type::String:
        return std::format("String({})", value.get<std::string>());
    case Value::Type::Image:
        return std::format("Image({})", value.get<std::string>());
    case Value::Type::Url:
        return std::format("Url({})", value.get<std::string>());
    case Value::Type::Color:
        return std::format("Color({})", value.get<Color::Color>().toString());
    case Value::Type::Integer:
        return std::format("Integer({})", value.get<int>());
    }
    return "Unknown";
}

uint8_t clampChannel(double value)
{
    if (value <= 0.0) {
        return 0;
    }
    if (value >= 255.0) {
        return 255;
    }
    return static_cast<uint8_t>(std::lround(value));
}

uint8_t alphaToChannel(double value0To1)
{
    if (value0To1 <= 0.0) {
        return 0;
    }
    if (value0To1 >= 1.0) {
        return 255;
    }
    return static_cast<uint8_t>(std::lround(value0To1 * 255.0));
}

// Standard HSL -> sRGB conversion.
void hslToRgb(double hue, double saturation, double lightness, uint8_t alpha, uint8_t out[4])
{
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    saturation = std::clamp(saturation, 0.0, 1.0);
    lightness = std::clamp(lightness, 0.0, 1.0);

    const double chroma = (1.0 - std::abs(2.0 * lightness - 1.0)) * saturation;
    const double huePrime = hue / 60.0;
    const double x = chroma * (1.0 - std::abs(std::fmod(huePrime, 2.0) - 1.0));
    const double m = lightness - chroma / 2.0;

    double r = 0, g = 0, b = 0;
    if (huePrime < 1.0) {
        r = chroma;
        g = x;
        b = 0;
    } else if (huePrime < 2.0) {
        r = x;
        g = chroma;
        b = 0;
    } else if (huePrime < 3.0) {
        r = 0;
        g = chroma;
        b = x;
    } else if (huePrime < 4.0) {
        r = 0;
        g = x;
        b = chroma;
    } else if (huePrime < 5.0) {
        r = x;
        g = 0;
        b = chroma;
    } else {
        r = chroma;
        g = 0;
        b = x;
    }

    out[0] = clampChannel((r + m) * 255.0);
    out[1] = clampChannel((g + m) * 255.0);
    out[2] = clampChannel((b + m) * 255.0);
    out[3] = alpha;
}

// Standard HWB -> sRGB conversion.
void hwbToRgb(double hue, double whiteness, double blackness, uint8_t alpha, uint8_t out[4])
{
    whiteness = std::clamp(whiteness, 0.0, 1.0);
    blackness = std::clamp(blackness, 0.0, 1.0);

    if (whiteness + blackness >= 1.0) {
        const double grey = (1.0 - whiteness - blackness) / 2.0;
        out[0] = out[1] = out[2] = clampChannel(grey * 255.0);
        out[3] = alpha;
        return;
    }

    uint8_t hsl[4];
    hslToRgb(hue, 1.0, 0.5, alpha, hsl);

    for (int i = 0; i < 3; ++i) {
        double channel = hsl[i] / 255.0;
        channel = channel * (1.0 - whiteness - blackness) + whiteness;
        out[i] = clampChannel(channel * 255.0);
    }
    out[3] = alpha;
}

// The standard CSS named colors.
struct NamedColor {
    const char *name;
    uint8_t r, g, b;
};

const NamedColor *lookupNamedColor(const std::string &lowerName)
{
    // clang-format off
    static constexpr auto colors = std::to_array<NamedColor>({
        {"aliceblue", 240, 248, 255}, {"antiquewhite", 250, 235, 215}, {"aqua", 0, 255, 255},
        {"aquamarine", 127, 255, 212}, {"azure", 240, 255, 255}, {"beige", 245, 245, 220},
        {"bisque", 255, 228, 196}, {"black", 0, 0, 0}, {"blanchedalmond", 255, 235, 205},
        {"blue", 0, 0, 255}, {"blueviolet", 138, 43, 226}, {"brown", 165, 42, 42},
        {"burlywood", 222, 184, 135}, {"cadetblue", 95, 158, 160}, {"chartreuse", 127, 255, 0},
        {"chocolate", 210, 105, 30}, {"coral", 255, 127, 80}, {"cornflowerblue", 100, 149, 237},
        {"cornsilk", 255, 248, 220}, {"crimson", 220, 20, 60}, {"cyan", 0, 255, 255},
        {"darkblue", 0, 0, 139}, {"darkcyan", 0, 139, 139}, {"darkgoldenrod", 184, 134, 11},
        {"darkgray", 169, 169, 169}, {"darkgreen", 0, 100, 0}, {"darkgrey", 169, 169, 169},
        {"darkkhaki", 189, 183, 107}, {"darkmagenta", 139, 0, 139}, {"darkolivegreen", 85, 107, 47},
        {"darkorange", 255, 140, 0}, {"darkorchid", 153, 50, 204}, {"darkred", 139, 0, 0},
        {"darksalmon", 233, 150, 122}, {"darkseagreen", 143, 188, 143}, {"darkslateblue", 72, 61, 139},
        {"darkslategray", 47, 79, 79}, {"darkslategrey", 47, 79, 79}, {"darkturquoise", 0, 206, 209},
        {"darkviolet", 148, 0, 211}, {"deeppink", 255, 20, 147}, {"deepskyblue", 0, 191, 255},
        {"dimgray", 105, 105, 105}, {"dimgrey", 105, 105, 105}, {"dodgerblue", 30, 144, 255},
        {"firebrick", 178, 34, 34}, {"floralwhite", 255, 250, 240}, {"forestgreen", 34, 139, 34},
        {"fuchsia", 255, 0, 255}, {"gainsboro", 220, 220, 220}, {"ghostwhite", 248, 248, 255},
        {"gold", 255, 215, 0}, {"goldenrod", 218, 165, 32}, {"gray", 128, 128, 128},
        {"green", 0, 128, 0}, {"greenyellow", 173, 255, 47}, {"grey", 128, 128, 128},
        {"honeydew", 240, 255, 240}, {"hotpink", 255, 105, 180}, {"indianred", 205, 92, 92},
        {"indigo", 75, 0, 130}, {"ivory", 255, 255, 240}, {"khaki", 240, 230, 140},
        {"lavender", 230, 230, 250}, {"lavenderblush", 255, 240, 245}, {"lawngreen", 124, 252, 0},
        {"lemonchiffon", 255, 250, 205}, {"lightblue", 173, 216, 230}, {"lightcoral", 240, 128, 128},
        {"lightcyan", 224, 255, 255}, {"lightgoldenrodyellow", 250, 250, 210}, {"lightgray", 211, 211, 211},
        {"lightgreen", 144, 238, 144}, {"lightgrey", 211, 211, 211}, {"lightpink", 255, 182, 193},
        {"lightsalmon", 255, 160, 122}, {"lightseagreen", 32, 178, 170}, {"lightskyblue", 135, 206, 250},
        {"lightslategray", 119, 136, 153}, {"lightslategrey", 119, 136, 153}, {"lightsteelblue", 176, 196, 222},
        {"lightyellow", 255, 255, 224}, {"lime", 0, 255, 0}, {"limegreen", 50, 205, 50},
        {"linen", 250, 240, 230}, {"magenta", 255, 0, 255}, {"maroon", 128, 0, 0},
        {"mediumaquamarine", 102, 205, 170}, {"mediumblue", 0, 0, 205}, {"mediumorchid", 186, 85, 211},
        {"mediumpurple", 147, 112, 219}, {"mediumseagreen", 60, 179, 113}, {"mediumslateblue", 123, 104, 238},
        {"mediumspringgreen", 0, 250, 154}, {"mediumturquoise", 72, 209, 204}, {"mediumvioletred", 199, 21, 133},
        {"midnightblue", 25, 25, 112}, {"mintcream", 245, 255, 250}, {"mistyrose", 255, 228, 225},
        {"moccasin", 255, 228, 181}, {"navajowhite", 255, 222, 173}, {"navy", 0, 0, 128},
        {"oldlace", 253, 245, 230}, {"olive", 128, 128, 0}, {"olivedrab", 107, 142, 35},
        {"orange", 255, 165, 0}, {"orangered", 255, 69, 0}, {"orchid", 218, 112, 214},
        {"palegoldenrod", 238, 232, 170}, {"palegreen", 152, 251, 152}, {"paleturquoise", 175, 238, 238},
        {"palevioletred", 219, 112, 147}, {"papayawhip", 255, 239, 213}, {"peachpuff", 255, 218, 185},
        {"peru", 205, 133, 63}, {"pink", 255, 192, 203}, {"plum", 222, 160, 221},
        {"powderblue", 176, 224, 230}, {"purple", 128, 0, 128}, {"rebeccapurple", 102, 51, 153},
        {"red", 255, 0, 0}, {"rosybrown", 188, 143, 143}, {"royalblue", 65, 105, 225},
        {"saddlebrown", 139, 69, 19}, {"salmon", 250, 128, 114}, {"sandybrown", 244, 164, 96},
        {"seagreen", 46, 139, 87}, {"seashell", 255, 245, 238}, {"sienna", 160, 82, 45},
        {"silver", 192, 192, 192}, {"skyblue", 135, 206, 235}, {"slateblue", 106, 90, 205},
        {"slategray", 112, 128, 144}, {"slategrey", 112, 128, 144}, {"snow", 255, 250, 250},
        {"springgreen", 0, 255, 127}, {"steelblue", 70, 130, 180}, {"tan", 210, 180, 140},
        {"teal", 0, 128, 128}, {"thistle", 216, 191, 216}, {"tomato", 255, 99, 71},
        {"turquoise", 64, 224, 208}, {"violet", 238, 130, 238}, {"wheat", 245, 222, 179},
        {"white", 255, 255, 255}, {"whitesmoke", 245, 245, 245}, {"yellow", 255, 255, 0},
        {"yellowgreen", 154, 205, 50},
    });
    // clang-format on

    for (const auto &color : colors) {
        if (lowerName == color.name) {
            return &color;
        }
    }
    return nullptr;
}

bool isHexDigit(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

// Converts a CSS hex color (without the leading '#') into RGBA components.
bool hexColorToRgba(const std::string &hex, uint8_t out[4])
{
    for (char c : hex) {
        if (!isHexDigit(c)) {
            return false;
        }
    }

    auto digit = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<uint8_t>(c - '0');
        }
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return static_cast<uint8_t>(lower - 'a' + 10);
    };

    switch (hex.size()) {
    case 3:
        out[0] = static_cast<uint8_t>(digit(hex[0]) * 16 + digit(hex[0]));
        out[1] = static_cast<uint8_t>(digit(hex[1]) * 16 + digit(hex[1]));
        out[2] = static_cast<uint8_t>(digit(hex[2]) * 16 + digit(hex[2]));
        out[3] = 255;
        return true;
    case 4:
        out[0] = static_cast<uint8_t>(digit(hex[0]) * 16 + digit(hex[0]));
        out[1] = static_cast<uint8_t>(digit(hex[1]) * 16 + digit(hex[1]));
        out[2] = static_cast<uint8_t>(digit(hex[2]) * 16 + digit(hex[2]));
        out[3] = static_cast<uint8_t>(digit(hex[3]) * 16 + digit(hex[3]));
        return true;
    case 6:
        for (size_t i = 0; i < 3; ++i) {
            out[i] = static_cast<uint8_t>(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
        }
        out[3] = 255;
        return true;
    case 8:
        for (size_t i = 0; i < 4; ++i) {
            out[i] = static_cast<uint8_t>(digit(hex[i * 2]) * 16 + digit(hex[i * 2 + 1]));
        }
        return true;
    default:
        return false;
    }
}

// Maps a dimension unit to its Dimension::Unit. Returns nullopt for units that
// are not (yet) supported, whether valid CSS units or not.
std::optional<Dimension::Unit> unitFromString(std::string_view unit)
{
    if (unit == "px")
        return {Dimension::Unit::Px};
    if (unit == "em")
        return {Dimension::Unit::Em};
    if (unit == "rem")
        return {Dimension::Unit::Rem};
    if (unit == "pt")
        return {Dimension::Unit::Pt};
    if (unit == "%")
        return {Dimension::Unit::Percent};
    if (unit == "deg")
        return {Dimension::Unit::Degrees};
    if (unit == "rad")
        return {Dimension::Unit::Radians};
    if (unit == "s")
        return {Dimension::Unit::Seconds};
    if (unit == "ms")
        return {Dimension::Unit::Milliseconds};
    return std::nullopt;
}

// A raw numeric component inside a color function: either a number or a
// percentage, or a hue with an angle unit.
struct ColorNumber {
    double value = 0.0;
    bool isPercentage = false;
};

bool tryColorNumber(Parser &parser, ColorNumber &out)
{
    if (parser.peek().type == Token::Type::Number) {
        out.value = parser.next().number;
        out.isPercentage = false;
        return true;
    }
    if (parser.peek().type == Token::Type::Percentage) {
        out.value = parser.next().number;
        out.isPercentage = true;
        return true;
    }
    return false;
}

double hueToDegrees(const Token &token)
{
    if (token.type == Token::Type::Dimension) {
        if (token.unit == "rad") {
            return token.number * 180.0 / M_PI;
        }
        if (token.unit == "grad") {
            return token.number * 0.9;
        }
        if (token.unit == "turn") {
            return token.number * 360.0;
        }
    }
    return token.number;
}

double channelValue(const ColorNumber &number)
{
    if (number.isPercentage) {
        return number.value * 255.0 / 100.0;
    }
    return number.value;
}

double alphaValue(const ColorNumber &number)
{
    if (number.isPercentage) {
        return number.value / 100.0;
    }
    return number.value;
}

// Parses the contents of rgb()/rgba(): `<r> <g> <b> [/ <a>]`, supporting both
// the legacy comma syntax and the modern space syntax.
std::expected<Value, ParseError> parseRgbFunction(Parser &parser)
{
    // Collect the numeric contents of the parentheses.
    struct Entry {
        std::optional<ColorNumber> number;
        bool isSeparator = false;
        char separator = ' ';
    };

    std::vector<Entry> entries;
    while (!parser.atEnd() && !parser.checkDelim(')')) {
        Entry entry;
        ColorNumber number;
        if (tryColorNumber(parser, number)) {
            // tryColorNumber consumed the number token already.
            entry.number = number;
            entries.push_back(entry);
            continue;
        }
        if (parser.checkDelim(',')) {
            parser.next();
            entry.isSeparator = true;
            entry.separator = ',';
            entries.push_back(entry);
            continue;
        }
        if (parser.checkDelim('/')) {
            parser.next();
            entry.isSeparator = true;
            entry.separator = '/';
            entries.push_back(entry);
            continue;
        }
        return std::unexpected(
            parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Unexpected token in rgb(): {}", parser.peek().toString())));
    }

    std::vector<ColorNumber> colorParts;
    std::optional<ColorNumber> alpha;
    bool afterSlash = false;

    for (const auto &entry : entries) {
        if (entry.isSeparator && entry.separator == '/') {
            afterSlash = true;
            continue;
        }
        if (entry.isSeparator) {
            continue;
        }
        if (afterSlash) {
            alpha = *entry.number;
        } else {
            colorParts.push_back(*entry.number);
        }
    }

    bool legacyCommaSyntax = std::any_of(entries.begin(), entries.end(), [](const Entry &entry) {
        return entry.isSeparator && entry.separator == ',';
    });

    double alpha01 = 1.0;

    if (legacyCommaSyntax) {
        // Legacy syntax: `rgb(r, g, b)` or `rgba(r, g, b, a)`. With four comma
        // separated numbers the fourth one is the alpha value.
        if (colorParts.size() == 4) {
            alpha01 = alphaValue(colorParts[3]);
            colorParts.resize(3);
        } else if (colorParts.size() == 3) {
            // rgba() with only three numbers: fully opaque.
        } else {
            return std::unexpected(
                parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("rgb() requires three or four components, got {}", colorParts.size())));
        }
    } else {
        // Modern syntax: `rgb(r g b [/ a])`.
        if (alpha.has_value()) {
            alpha01 = alphaValue(*alpha);
        }
        if (colorParts.size() != 3) {
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue,
                                                    std::format("rgb() requires exactly three color components, got {}", colorParts.size())));
        }
    }

    if (colorParts.size() != 3) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "rgb() requires exactly three color components"));
    }

    return {makeRgbaColor(clampChannel(channelValue(colorParts[0])),
                          clampChannel(channelValue(colorParts[1])),
                          clampChannel(channelValue(colorParts[2])),
                          alphaToChannel(alpha01))};
}

// Parses the contents of hsl()/hsla(): `<h> <s> <l> [/ <a>]` or the legacy
// comma syntax `<h>, <s>, <l>, <a>`.
std::expected<Value, ParseError> parseHslFunction(Parser &parser)
{
    double hue = 0.0;
    {
        // The hue can be a plain number, which is interpreted as degrees, or a
        // dimension with an angle unit.
        const Token &token = parser.peek();
        if (token.type == Token::Type::Number) {
            hue = token.number;
            parser.next();
        } else if (token.type == Token::Type::Dimension) {
            hue = hueToDegrees(token);
            parser.next();
        } else {
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "Expected a hue value in hsl()"));
        }
    }

    std::vector<ColorNumber> parts;
    std::optional<ColorNumber> alpha;
    bool afterSlash = false;

    while (!parser.atEnd() && !parser.checkDelim(')')) {
        ColorNumber number;
        if (tryColorNumber(parser, number)) {
            if (afterSlash) {
                alpha = number;
            } else {
                parts.push_back(number);
            }
            continue;
        }
        if (parser.checkDelim(',')) {
            parser.next();
            continue;
        }
        if (parser.checkDelim('/')) {
            parser.next();
            afterSlash = true;
            continue;
        }
        return std::unexpected(
            parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Unexpected token in hsl(): {}", parser.peek().toString())));
    }

    if (parts.size() < 2 || parts.size() > 3) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "hsl() requires saturation and lightness values"));
    }

    double alpha01 = 1.0;
    if (parts.size() == 3 && !alpha.has_value()) {
        alpha01 = alphaValue(parts[2]);
        parts.resize(2);
    }
    if (alpha.has_value()) {
        alpha01 = alphaValue(*alpha);
    }

    uint8_t rgba[4];
    hslToRgb(hue, parts[0].value / 100.0, parts[1].value / 100.0, alphaToChannel(alpha01), rgba);
    return {makeRgbaColor(rgba[0], rgba[1], rgba[2], rgba[3])};
}

// Parses the contents of hwb(): `<h> <w> <b> [/ <a>]`.
std::expected<Value, ParseError> parseHwbFunction(Parser &parser)
{
    const Token &token = parser.peek();
    double hue = 0.0;
    if (token.type == Token::Type::Number) {
        hue = token.number;
        parser.next();
    } else if (token.type == Token::Type::Dimension) {
        hue = hueToDegrees(token);
        parser.next();
    } else {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "Expected a hue value in hwb()"));
    }

    ColorNumber whiteness, blackness;
    if (!tryColorNumber(parser, whiteness) || !tryColorNumber(parser, blackness)) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "hwb() requires whitening and blackening percentages"));
    }

    double alpha01 = 1.0;
    if (parser.checkDelim('/')) {
        parser.next();
        ColorNumber alpha;
        if (!tryColorNumber(parser, alpha)) {
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "Expected an alpha value after '/'"));
        }
        alpha01 = alphaValue(alpha);
    }

    uint8_t rgba[4];
    hwbToRgb(hue, whiteness.value / 100.0, blackness.value / 100.0, alphaToChannel(alpha01), rgba);
    return {makeRgbaColor(rgba[0], rgba[1], rgba[2], rgba[3])};
}

/*!
 * Parses a single value component at the current position.
 *
 * Returns:
 *  - a non-empty vector of values on success;
 *  - an empty vector when the current token does not start a value component,
 *    without consuming anything;
 *  - an error on a hard failure.
 */
std::expected<std::vector<Value>, ParseError> parseValueComponent(Parser &parser)
{
    std::vector<Value> result;

    if (parser.atEnd()) {
        return {result};
    }

    const Token &token = parser.peek();

    switch (token.type) {
    case Token::Type::Number: {
        parser.next();
        if (std::trunc(token.number) == token.number) {
            result.push_back(makeIntegerValue(static_cast<int>(token.number)));
        } else {
            result.push_back(makeNumberValue(token.number));
        }
        return {result};
    }
    case Token::Type::Percentage: {
        parser.next();
        result.push_back(makeDimensionValue(Dimension::Unit::Percent, token.number));
        return {result};
    }
    case Token::Type::Dimension: {
        auto unit = unitFromString(token.unit);
        if (!unit.has_value()) {
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Invalid unit for dimension: {}", token.unit)));
        }
        parser.next();
        result.push_back(makeDimensionValue(*unit, token.number));
        return {result};
    }
    case Token::Type::QuotedString: {
        result.push_back(makeStringValue(token.text));
        parser.next();
        return {result};
    }
    case Token::Type::Ident: {
        const std::string lower = asciiLower(token.text);
        if (lower == "currentcolor") {
            parser.next();
            Color::Color color{Color::Color::Type::Custom, Color::CustomColorData{"currentcolor", {}}};
            result.push_back(Value{Value::Type::Color, color});
            return {result};
        }
        if (const NamedColor *named = lookupNamedColor(lower)) {
            parser.next();
            result.push_back(makeRgbaColor(named->r, named->g, named->b, 255));
            return {result};
        }
        // Plain identifiers are stored as strings; validation against the
        // property syntax decides whether that is allowed.
        result.push_back(makeStringValue(token.text));
        parser.next();
        return {result};
    }
    case Token::Type::Url: {
        result.push_back(makeUrlValue(token.url));
        parser.next();
        return {result};
    }
    case Token::Type::Hash: {
        uint8_t rgba[4];
        if (hexColorToRgba(token.text, rgba)) {
            parser.next();
            result.push_back(makeRgbaColor(rgba[0], rgba[1], rgba[2], rgba[3]));
        }
        // When it is not a valid hex color, leave the token alone; the caller
        // produces the generic parse error.
        return {result};
    }
    case Token::Type::Function: {
        const std::string lower = asciiLower(token.text);

        if (lower == "rgb" || lower == "rgba") {
            auto value = parser.parseFunctionCall([](Parser &p) -> std::expected<Value, ParseError> {
                return parseRgbFunction(p);
            });
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            return {std::vector<Value>{*value}};
        }
        if (lower == "hsl" || lower == "hsla") {
            auto value = parser.parseFunctionCall([](Parser &p) -> std::expected<Value, ParseError> {
                return parseHslFunction(p);
            });
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            return {std::vector<Value>{*value}};
        }
        if (lower == "hwb") {
            auto value = parser.parseFunctionCall([](Parser &p) -> std::expected<Value, ParseError> {
                return parseHwbFunction(p);
            });
            if (!value.has_value()) {
                return std::unexpected(value.error());
            }
            return {std::vector<Value>{*value}};
        }

        if (auto function = propertyFunction(lower)) {
            auto values = parser.parseFunctionCall([function](Parser &p) -> PropertyFunctionResult {
                return (*function)(p);
            });
            return values;
        }

        // An unknown function is an error, like in the previous parser.
        parser.next(); // Consume the function token so parsing can continue.
        return std::unexpected(parser.makeError(ParseError::Kind::UnknownFunction, std::format("Unknown function {}", lower)));
    }
    default:
        return {result};
    }
}

} // namespace

// --- Syntax validation -------------------------------------------------------------

enum class ListType : uint8_t {
    NotAList,
    SpaceSeparated,
    CommaSeparated
};

struct ParsedValues {
    ListType listType = ListType::NotAList;
    std::vector<Value> values;
};

namespace
{

using ValidationResult = std::expected<size_t, std::string>;

bool isLengthUnit(Dimension::Unit unit)
{
    switch (unit) {
    case Dimension::Unit::Px:
    case Dimension::Unit::Em:
    case Dimension::Unit::Rem:
    case Dimension::Unit::Pt:
        return true;
    default:
        return false;
    }
}

bool isAngleUnit(Dimension::Unit unit)
{
    switch (unit) {
    case Dimension::Unit::Degrees:
    case Dimension::Unit::Radians:
        return true;
    default:
        return false;
    }
}

ValidationResult validateDataType(DataType type, const std::vector<Value> &values, size_t index)
{
    if (index >= values.size()) {
        return std::unexpected("Expected a value");
    }

    const Value &value = values[index];

    auto expectDimension = [&value, index](bool (*predicate)(Dimension::Unit), std::string_view what) -> ValidationResult {
        if (value.type() == Value::Type::Dimension) {
            const auto &dimension = value.get<Dimension>();
            if (predicate(dimension.unit())) {
                return {index + 1};
            }
        }
        return std::unexpected(std::format("Expected {}, got {}", what, valueDisplay(value)));
    };

    switch (type) {
    case DataType::Length:
        return expectDimension(
            [](Dimension::Unit unit) {
                return isLengthUnit(unit);
            },
            "Length");
    case DataType::Number:
        // A plain integer value is a valid number as well.
        if (value.type() == Value::Type::Integer) {
            return {index + 1};
        }
        return expectDimension(
            [](Dimension::Unit unit) {
                return unit == Dimension::Unit::Number;
            },
            "Number");
    case DataType::Percentage:
        return expectDimension(
            [](Dimension::Unit unit) {
                return unit == Dimension::Unit::Percent;
            },
            "Percentage");
    case DataType::LengthPercentage:
        return expectDimension(
            [](Dimension::Unit unit) {
                return isLengthUnit(unit) || unit == Dimension::Unit::Percent;
            },
            "Length or Percentage");
    case DataType::Angle:
        return expectDimension(
            [](Dimension::Unit unit) {
                return isAngleUnit(unit);
            },
            "Angle");
    case DataType::String:
        if (value.type() == Value::Type::String) {
            return {index + 1};
        }
        return std::unexpected(std::format("Expected String, got {}", valueDisplay(value)));
    case DataType::Color:
        if (value.type() == Value::Type::Color) {
            return {index + 1};
        }
        return std::unexpected(std::format("Expected Color, got {}", valueDisplay(value)));
    case DataType::Integer:
        if (value.type() == Value::Type::Integer) {
            return {index + 1};
        }
        return std::unexpected(std::format("Expected Integer, got {}", valueDisplay(value)));
    case DataType::Url:
        if (value.type() == Value::Type::Url) {
            return {index + 1};
        }
        return std::unexpected(std::format("Expected URL, got {}", valueDisplay(value)));
    default:
        return std::unexpected(std::format("Unhandled data type {}", std::string_view{dataTypeName(type)}));
    }
}

ValidationResult validateKeyword(const std::string &keyword, const std::vector<Value> &values, size_t index)
{
    if (index >= values.size()) {
        return std::unexpected("Expected a keyword");
    }

    const Value &value = values[index];
    if (value.type() == Value::Type::String) {
        if (value.get<std::string>() == keyword) {
            return {index + 1};
        }
        return std::unexpected(std::format("Unexpected keyword {}", value.get<std::string>()));
    }

    return std::unexpected(std::format("{} is not a keyword", valueDisplay(value)));
}

ValidationResult validateList(DataType type, const std::vector<Value> &values, size_t index, size_t minimum, size_t maximum)
{
    size_t count = 0;
    size_t current = index;

    while (current < values.size()) {
        auto result = validateDataType(type, values, current);
        if (result.has_value()) {
            current = *result;
            count++;
            if (count == maximum) {
                break;
            }
        } else {
            // With a minimum of zero, a non-matching trailing value simply ends the list.
            if (minimum == 0) {
                break;
            }
            return result;
        }
    }

    if (count < minimum) {
        return std::unexpected(std::format("Expected at least {} values of type {}", minimum, std::string_view{dataTypeName(type)}));
    }
    if (count > maximum) {
        return std::unexpected(std::format("Expected at most {} values of type {}", maximum, std::string_view{dataTypeName(type)}));
    }
    return {current};
}

struct ValidateContext {
    const std::vector<SyntaxAlternatives> *remaining;
    size_t index;
    ListType listType;
};

ValidationResult validateComponent(const SyntaxComponent &component, const std::vector<Value> &values, ValidateContext &context);
ValidationResult validateGroup(const SyntaxGroup &group, const std::vector<Value> &values, ValidateContext &context);

ValidationResult validateAlternatives(const SyntaxAlternatives &alternatives, const std::vector<Value> &values, ValidateContext &context)
{
    return std::visit(
        [&](const auto &entry) -> ValidationResult {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, SyntaxComponent>) {
                return validateComponent(entry, values, context);
            } else if constexpr (std::is_same_v<T, SyntaxGroup>) {
                return validateGroup(entry, values, context);
            } else {
                for (size_t alt = 0; alt < entry.size(); ++alt) {
                    ValidateContext inner{.remaining = context.remaining, .index = context.index, .listType = context.listType};
                    auto result = validateGroup(entry[alt], values, inner);
                    if (result.has_value()) {
                        return result;
                    }
                }
                return std::unexpected("None of the alternatives matched");
            }
        },
        alternatives.data);
}

ValidationResult validateGroup(const SyntaxGroup &group, const std::vector<Value> &values, ValidateContext &context)
{
    return std::visit(
        [&](const auto &entry) -> ValidationResult {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, SyntaxComponent>) {
                auto r = validateComponent(entry, values, context);
                return r;
            } else {
                // A parenthesized expression matches when its contents match
                // starting at the current value index.
                ValidateContext inner{.remaining = context.remaining, .index = context.index, .listType = context.listType};
                size_t current = context.index;
                for (const auto &alternative : entry) {
                    ValidateContext altContext{.remaining = inner.remaining, .index = current, .listType = inner.listType};
                    auto result = validateAlternatives(alternative, values, altContext);
                    if (!result.has_value()) {
                        return result;
                    }
                    current = *result;
                }
                return {current};
            }
        },
        group.data);
}

ValidationResult validateComponent(const SyntaxComponent &component, const std::vector<Value> &values, ValidateContext &context)
{
    return std::visit(
        [&](const auto &entry) -> ValidationResult {
            using T = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<T, ComponentComma>) {
                return {context.index};
            } else if constexpr (std::is_same_v<T, ComponentDataType>) {
                return validateDataType(entry.type, values, context.index);
            } else if constexpr (std::is_same_v<T, ComponentKeyword>) {
                return validateKeyword(entry.name, values, context.index);
            } else if constexpr (std::is_same_v<T, ComponentSpaceSeparatedList>) {
                if (context.listType == ListType::CommaSeparated) {
                    return std::unexpected("Expected space separated list, got comma separated");
                }
                return validateList(entry.type, values, context.index, 1, SIZE_MAX);
            } else if constexpr (std::is_same_v<T, ComponentCommaSeparatedList>) {
                if (context.listType == ListType::SpaceSeparated) {
                    return std::unexpected("Expected comma separated list, got space separated");
                }
                return validateList(entry.type, values, context.index, 1, SIZE_MAX);
            } else if constexpr (std::is_same_v<T, ComponentRepeat>) {
                if (context.listType == ListType::CommaSeparated) {
                    return std::unexpected("Expected space separated list, got comma separated");
                }
                return validateList(entry.type, values, context.index, entry.minimum, entry.maximum);
            } else {
                return std::unexpected("Unknown syntax component");
            }
        },
        component);
}

std::expected<void, std::string> validateSyntax(const ParsedPropertySyntax &syntax, const ParsedValues &parsed)
{
    if (syntax.kind == ParsedPropertySyntax::Kind::Empty || syntax.kind == ParsedPropertySyntax::Kind::Universal) {
        return {};
    }

    ValidateContext context{.remaining = &syntax.expression, .index = 0, .listType = parsed.listType};
    size_t index = 0;

    for (size_t i = 0; i < syntax.expression.size(); ++i) {
        ValidateContext altContext{.remaining = context.remaining, .index = index, .listType = context.listType};
        auto result = validateAlternatives(syntax.expression[i], parsed.values, altContext);
        if (!result.has_value()) {
            // Build a message describing what was expected at this point.
            std::vector<SyntaxAlternatives> remaining(syntax.expression.begin() + i, syntax.expression.end());
            std::string expected;
            for (const auto &alternative : remaining) {
                if (!expected.empty()) {
                    expected += " ";
                }
                expected += alternativesToString(alternative);
            }
            std::string actual;
            for (const auto &value : parsed.values) {
                if (!actual.empty()) {
                    actual += " | ";
                }
                actual += valueDisplay(value);
            }
            return std::unexpected(std::format("Unexpected value, expected: {}, got: [{}]", expected, actual));
        }
        index = *result;
    }

    if (index != parsed.values.size()) {
        return std::unexpected(std::format("Received too many values, {} value(s) were not consumed", parsed.values.size() - index));
    }

    return {};
}

} // namespace

std::expected<ParsedValues, ParseError> parseValues(const ParsedPropertySyntax &syntax, Parser &parser)
{
    ParsedValues result;
    bool commaSeparated = false;

    while (!parser.atEnd()) {
        if (parser.checkDelim(';') || parser.checkDelim('}') || parser.checkDelim('{') || parser.checkDelim(')') || parser.checkDelim('!')) {
            break;
        }

        if (parser.consumeDelim(',')) {
            commaSeparated = true;
            continue;
        }

        auto component = parseValueComponent(parser);
        if (!component.has_value()) {
            return std::unexpected(component.error());
        }
        if (component->empty()) {
            return std::unexpected(
                parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Could not parse input: unexpected {}", parser.peek().toString())));
        }

        result.values.insert(result.values.end(), std::make_move_iterator(component->begin()), std::make_move_iterator(component->end()));
    }

    if (result.values.size() == 1) {
        result.listType = ListType::NotAList;
    } else if (commaSeparated) {
        result.listType = ListType::CommaSeparated;
    } else {
        result.listType = ListType::SpaceSeparated;
    }

    auto validation = validateSyntax(syntax, result);
    if (!validation.has_value()) {
        ParseError error = parser.makeError(ParseError::Kind::PropertyValueDoesNotMatchSyntax, validation.error());
        return std::unexpected(error);
    }

    return {std::move(result)};
}

// --- Registries ------------------------------------------------------------------

namespace
{

struct DefinitionRegistry {
    std::shared_mutex mutex;
    std::vector<std::shared_ptr<PropertyDefinition>> definitions;
};

DefinitionRegistry &definitionRegistry()
{
    static DefinitionRegistry registry;
    return registry;
}

struct FunctionRegistry {
    std::shared_mutex mutex;
    std::map<std::string, PropertyFunction> functions;
};

} // namespace

std::shared_ptr<const PropertyDefinition> propertyDefinition(const std::string &name)
{
    auto &registry = definitionRegistry();
    std::shared_lock lock(registry.mutex);
    for (const auto &definition : registry.definitions) {
        if (definition->name == name) {
            return definition;
        }
    }
    return nullptr;
}

bool addPropertyDefinition(const std::shared_ptr<PropertyDefinition> &definition)
{
    auto &registry = definitionRegistry();
    std::unique_lock lock(registry.mutex);
    for (const auto &existing : registry.definitions) {
        if (existing->name == definition->name) {
            return false; // The first definition of a property wins.
        }
    }
    registry.definitions.push_back(definition);
    return true;
}

namespace
{

// Consumes tokens until the closing parenthesis of the current function call
// without interpreting them.
void skipToClosingParen(Parser &parser)
{
    int depth = 1;
    while (!parser.atEnd()) {
        if (parser.checkDelim(')')) {
            depth--;
            if (depth == 0) {
                return; // The wrapper consumes the `)`.
            }
        } else if (parser.checkDelim('(') || parser.check(Token::Type::Function)) {
            depth++;
        }
        parser.next();
    }
}

// `var(<name>, <fallback>?)`: resolves a custom property reference. When the
// property is defined, its initial value is substituted and any fallback is
// ignored; otherwise the fallback is parsed, as in CSS var().
PropertyFunctionResult varFunction(Parser &parser)
{
    auto name = parser.expectIdent();
    if (!name.has_value()) {
        return std::unexpected(name.error());
    }

    if (auto definition = propertyDefinition(*name)) {
        skipToClosingParen(parser);
        return {definition->initial};
    }

    if (!parser.checkDelim(',')) {
        return std::unexpected(parser.makeError(ParseError::Kind::UnknownProperty, std::format("No custom property {} was defined", *name)));
    }
    parser.next(); // Consume the comma.

    auto values = parseValues(universalSyntax(), parser);
    if (!values.has_value()) {
        return std::unexpected(values.error());
    }
    return {std::move(values->values)};
}

// `mix(<color>, <color>, <number>)`: builds a modified color that blends two colors.
PropertyFunctionResult mixFunction(Parser &parser)
{
    auto parsed = parseValues(syntaxFromText("<color>, <color>, <number>"), parser);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }

    if (parsed->values.size() != 3) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "mix() requires exactly three arguments"));
    }

    auto first = std::make_shared<Color::Color>(parsed->values[0].get<Color::Color>());
    auto second = std::make_shared<Color::Color>(parsed->values[1].get<Color::Color>());
    const auto amount = parsed->values[2].get<Dimension>();

    Color::ModifiedColorData data{
        Color::ModifiedColorData::Operation::Mix,
        first,
        Color::MixOperationData{second, amount.value()},
    };

    Color::Color result{Color::Color::Type::Modified, data};
    return {std::vector<Value>{Value{Value::Type::Color, result}}};
}

// `custom-color(<source>, <argument>...)`: an opaque color resolved by the consumer.
PropertyFunctionResult customColorFunction(Parser &parser)
{
    auto parsed = parseValues(syntaxFromText("<string>, <string>#"), parser);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }

    if (parsed->values.size() < 1) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "custom-color() requires at least a source argument"));
    }

    const auto source = parsed->values[0].get<std::string>();
    std::vector<std::string> arguments;
    for (size_t i = 1; i < parsed->values.size(); ++i) {
        arguments.push_back(parsed->values[i].get<std::string>());
    }

    Color::CustomColorData data{source, arguments};
    Color::Color result{Color::Color::Type::Custom, data};
    return {std::vector<Value>{Value{Value::Type::Color, result}}};
}

// `modify-color(<color> (add | subtract | multiply) <color>)` and
// `modify-color(<color> set-alpha <number>)`.
PropertyFunctionResult modifyColorFunction(Parser &parser)
{
    auto parsed = parseValues(syntaxFromText("<color> ((add | subtract | multiply) <color> | set-alpha <number>)"), parser);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }

    if (parsed->values.size() != 3) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidPropertyValue, "modify-color() requires exactly three arguments"));
    }

    const auto operation = parsed->values[1].get<std::string>();
    auto base = std::make_shared<Color::Color>(parsed->values[0].get<Color::Color>());

    if (operation == "add" || operation == "subtract" || operation == "multiply") {
        if (parsed->values[2].type() != Value::Type::Color) {
            return std::unexpected(
                parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Expected a color, got {}", valueDisplay(parsed->values[2]))));
        }
        auto other = std::make_shared<Color::Color>(parsed->values[2].get<Color::Color>());

        Color::ModifiedColorData::Operation op = Color::ModifiedColorData::Operation::Add;
        if (operation == "subtract") {
            op = Color::ModifiedColorData::Operation::Subtract;
        } else if (operation == "multiply") {
            op = Color::ModifiedColorData::Operation::Multiply;
        }

        Color::ModifiedColorData data{op, base, other};
        Color::Color result{Color::Color::Type::Modified, data};
        return {std::vector<Value>{Value{Value::Type::Color, result}}};
    }

    if (operation == "set-alpha") {
        // The argument is an alpha value in the range 0..1, like in the
        // previous Rust implementation (`value * 255.0`). It may be stored as
        // an integer or as a unit-less dimension depending on how it was written.
        double number = 0.0;
        if (parsed->values[2].type() == Value::Type::Integer) {
            number = static_cast<double>(parsed->values[2].get<int>());
        } else {
            number = static_cast<double>(parsed->values[2].get<Dimension>().value());
        }
        const auto alpha = static_cast<std::optional<uint8_t>>(static_cast<uint8_t>(std::clamp(number * 255.0, 0.0, 255.0)));

        Color::SetOperationData setData{std::nullopt, std::nullopt, std::nullopt, alpha};
        Color::ModifiedColorData data{Color::ModifiedColorData::Operation::Set, base, setData};
        Color::Color result{Color::Color::Type::Modified, data};
        return {std::vector<Value>{Value{Value::Type::Color, result}}};
    }

    return std::unexpected(parser.makeError(ParseError::Kind::Unknown, "Unexpected modify-color argument"));
}

bool initFunctionRegistry(FunctionRegistry &registry)
{
    registry.functions["var"] = varFunction;
    registry.functions["mix"] = mixFunction;
    registry.functions["custom-color"] = customColorFunction;
    registry.functions["modify-color"] = modifyColorFunction;
    return true;
}

FunctionRegistry &functionRegistry()
{
    static FunctionRegistry registry;

    // The registry holds a mutex, so it cannot be constructed from a
    // pre-filled temporary. Register the built-in functions in place instead.
    static const bool initialized = initFunctionRegistry(registry);
    (void)initialized;

    return registry;
}

} // namespace

std::optional<PropertyFunction> propertyFunction(std::string_view name)
{
    auto &registry = functionRegistry();
    std::shared_lock lock(registry.mutex);
    auto it = registry.functions.find(std::string(name));
    if (it == registry.functions.end()) {
        return std::nullopt;
    }
    return {it->second};
}

bool addPropertyFunction(std::string_view name, PropertyFunction function)
{
    auto &registry = functionRegistry();
    std::unique_lock lock(registry.mutex);
    const std::string key{name};
    if (registry.functions.contains(key)) {
        return false;
    }
    registry.functions[key] = std::move(function);
    return true;
}

// --- Selectors -------------------------------------------------------------------

namespace
{

using SelectorParts = std::vector<SelectorPart>;

SelectorPart makeSelectorPart(SelectorPart::Kind kind, const std::string &value = {})
{
    if (value.empty()) {
        return SelectorPart{kind, Value{}};
    }
    return SelectorPart{kind, Value{Value::Type::String, value}};
}

AttributeMatch::Operator attributeOperatorFromToken(const std::string &opText)
{
    if (opText == "=")
        return AttributeMatch::Operator::Equals;
    if (opText == "~=")
        return AttributeMatch::Operator::Includes;
    if (opText == "^=")
        return AttributeMatch::Operator::Prefixed;
    if (opText == "$=")
        return AttributeMatch::Operator::Suffixed;
    if (opText == "*=")
        return AttributeMatch::Operator::Substring;
    if (opText == "|=")
        return AttributeMatch::Operator::DashMatch;
    return AttributeMatch::Operator::None;
}

// Parses a single selector into its parts, in source order. Stops at `,`, `{`,
// `;` or end of input without consuming the stop token.
std::expected<SelectorParts, ParseError> parseOneSelector(Parser &parser, bool allowRelative)
{
    SelectorParts parts;
    bool pendingDescendant = false;

    auto flushDescendant = [&parts, &pendingDescendant]() {
        if (pendingDescendant && !parts.empty() && !parts.back().isCombinator()) {
            parts.push_back(makeSelectorPart(SelectorPart::Kind::DescendantCombinator));
        }
        pendingDescendant = false;
    };

    while (!parser.atEnd()) {
        const Token &token = parser.peek();

        if (token.isDelim(',') || token.isDelim('{') || token.isDelim(';')) {
            break;
        }

        // Combinator tokens take precedence over the pending whitespace flag.
        if (token.text == ">" || token.text == ">>>") {
            if (parts.empty()) {
                // A leading combinator makes the selector relative to its parent.
                // That is only valid inside a nested rule; at top level it dangles.
                if (!allowRelative) {
                    return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Dangling combinator"));
                }
                pendingDescendant = false;
                parts.push_back(makeSelectorPart(token.text == ">" ? SelectorPart::Kind::ChildCombinator : SelectorPart::Kind::PartCombinator));
                parser.next();
                continue;
            }
            if (parts.back().kind() == SelectorPart::Kind::DescendantCombinator) {
                parts.pop_back(); // The explicit combinator replaces the implicit descendant one.
            }
            pendingDescendant = false;
            parts.push_back(makeSelectorPart(token.text == ">" ? SelectorPart::Kind::ChildCombinator : SelectorPart::Kind::PartCombinator));
            parser.next();
            continue;
        }

        if (token.isDelim('+') || token.isDelim('~')) {
            std::cerr << "Warning: Combinator '" << token.text << "' not implemented" << std::endl;
            parser.next();
            pendingDescendant = false;
            continue;
        }

        // Whitespace between two compounds implies a descendant combinator.
        if (token.wsBefore && !parts.empty()) {
            pendingDescendant = true;
        }

        if (token.isDelim('&')) {
            flushDescendant();
            parts.push_back(makeSelectorPart(SelectorPart::Kind::RelativeParent));
            parser.next();
            continue;
        }

        if (token.isDelim('*')) {
            flushDescendant();
            parts.push_back(makeSelectorPart(SelectorPart::Kind::AnyElement));
            parser.next();
            continue;
        }

        if (token.type == Token::Type::Hash) {
            flushDescendant();
            parts.push_back(makeSelectorPart(SelectorPart::Kind::Id, token.text));
            parser.next();
            continue;
        }

        if (token.isDelim('.')) {
            if (parser.peek(1).type == Token::Type::Ident) {
                flushDescendant();
                parts.push_back(makeSelectorPart(SelectorPart::Kind::Class, parser.peek(1).text));
                parser.next();
                parser.next();
                continue;
            }
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Expected a class name after '.'"));
        }

        if (token.isDelim('[')) {
            flushDescendant();
            parser.next(); // Consume '['.

            auto name = parser.expectIdent();
            if (!name.has_value()) {
                return std::unexpected(parser.ownedError(name.error()));
            }

            if (parser.consumeDelim(']')) {
                AttributeMatch match{*name, AttributeMatch::Operator::Exists, Value{}};
                parts.push_back(SelectorPart{SelectorPart::Kind::Attribute, Value{}, match});
                continue;
            }

            const Token opToken = parser.next();
            auto op = attributeOperatorFromToken(opToken.text);
            if (op == AttributeMatch::Operator::None) {
                return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, std::format("Unknown attribute operator {}", opToken.text)));
            }

            std::string valueText;
            while (!parser.atEnd() && !parser.checkDelim(']')) {
                const Token &valueToken = parser.peek();
                if (valueToken.type == Token::Type::QuotedString || valueToken.type == Token::Type::Ident) {
                    valueText += valueToken.text;
                } else if (valueToken.type == Token::Type::Number || valueToken.type == Token::Type::Dimension) {
                    valueText += valueToken.toString();
                } else {
                    return std::unexpected(
                        parser.makeError(ParseError::Kind::InvalidSelectors, std::format("Unexpected token {} in attribute selector", valueToken.toString())));
                }
                parser.next();
            }

            if (!parser.consumeDelim(']')) {
                return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Expected ']' to close attribute selector"));
            }

            AttributeMatch match{*name, op, Value{Value::Type::String, valueText}};
            parts.push_back(SelectorPart{SelectorPart::Kind::Attribute, Value{}, match});
            continue;
        }

        if (token.isDelim(':')) {
            // Pseudo-class or pseudo-element.
            const Token &afterColon = parser.peek(1);

            if (afterColon.isDelim(':')) {
                // `::part(name)` is supported; other pseudo-elements are skipped.
                if (parser.peek(2).type == Token::Type::Function && asciiLower(parser.peek(2).text) == "part") {
                    flushDescendant();
                    parser.next(); // ':'
                    parser.next(); // ':'
                    parser.next(); // 'part(' function token (the `(` is part of the token)
                    auto partName = parser.expectIdent();
                    if (!partName.has_value()) {
                        return std::unexpected(parser.ownedError(partName.error()));
                    }
                    if (!parser.consumeDelim(')')) {
                        return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Expected ')' after ::part() name"));
                    }
                    parts.push_back(makeSelectorPart(SelectorPart::Kind::Part, *partName));
                    continue;
                }

                if (parser.peek(2).type == Token::Type::Ident) {
                    std::cerr << "Warning: pseudo-element ::" << parser.peek(2).text << " not implemented" << std::endl;
                    parser.next();
                    parser.next();
                    parser.next();
                    pendingDescendant = false;
                    continue;
                }

                return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Expected a pseudo-element name after '::'"));
            }

            if (afterColon.type == Token::Type::Ident) {
                flushDescendant();
                const std::string name = afterColon.text;
                parser.next(); // ':'
                parser.next(); // name
                if (asciiLower(name) == "root") {
                    parts.push_back(makeSelectorPart(SelectorPart::Kind::DocumentRoot));
                } else {
                    parts.push_back(makeSelectorPart(SelectorPart::Kind::PseudoClass, name));
                }
                continue;
            }

            if (afterColon.type == Token::Type::Function) {
                if (asciiLower(afterColon.text) == "state") {
                    flushDescendant();
                    parser.next(); // ':'
                    // `:state(name)` stores the state name as the value, like any
                    // other pseudo-class the name alone is not very useful.
                    auto result = parser.parseFunctionCall([](Parser &p) -> std::expected<std::string, ParseError> {
                        auto ident = p.expectIdent();
                        if (!ident.has_value()) {
                            return std::unexpected(ident.error());
                        }
                        return {std::string{*ident}};
                    });
                    if (!result.has_value()) {
                        return std::unexpected(result.error());
                    }
                    parts.push_back(makeSelectorPart(SelectorPart::Kind::PseudoClass, *result));
                    continue;
                }

                return std::unexpected(
                    parser.makeError(ParseError::Kind::InvalidSelectors, std::format("Unsupported pseudo-class function {}", afterColon.text)));
            }

            return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Expected a pseudo-class name after ':'"));
        }

        // A plain identifier is a type (element) selector, like `button`.
        if (token.type == Token::Type::Ident) {
            flushDescendant();
            parts.push_back(makeSelectorPart(SelectorPart::Kind::Type, token.text));
            parser.next();
            continue;
        }

        // Anything else that reaches this point is an unexpected token for a
        // selector.
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, std::format("Unexpected token {} in selector", token.toString())));
    }

    // A combinator at the end of the selector is dangling.
    if (!parts.empty() && parts.back().isCombinator()) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Dangling combinator"));
    }

    return {parts};
}

std::expected<std::vector<Selector>, ParseError> parseSelectorList(Parser &parser, bool allowRelative)
{
    std::vector<Selector> selectors;

    while (true) {
        auto parts = parseOneSelector(parser, allowRelative);
        if (!parts.has_value()) {
            return std::unexpected(parts.error());
        }

        if (parts->empty()) {
            if (!selectors.empty() && !parser.checkDelim(',')) {
                // Trailing comma before the block, tolerated like CSS parsers do.
                break;
            }
            if (selectors.empty()) {
                return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Empty selector"));
            }
            return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Empty selector"));
        }

        selectors.emplace_back(*parts);

        if (parser.checkDelim(',')) {
            parser.next();
            continue;
        }
        break;
    }

    if (selectors.empty()) {
        return std::unexpected(parser.makeError(ParseError::Kind::InvalidSelectors, "Empty selector"));
    }

    return {selectors};
}

// Combines a (possibly `&`-relative) child selector with its parent selector.
Selector combineSelectors(const Selector &child, const Selector &parent)
{
    SelectorParts parts(child.parts().begin(), child.parts().end());

    if (parent.parts().empty()) {
        return Selector{parts};
    }

    std::vector<size_t> relativeIndices;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].kind() == SelectorPart::Kind::RelativeParent) {
            relativeIndices.push_back(i);
        }
    }

    if (relativeIndices.empty()) {
        // Without an `&`, the child selector applies underneath the parent.
        SelectorParts combined(parent.parts().begin(), parent.parts().end());
        combined.push_back(makeSelectorPart(SelectorPart::Kind::DescendantCombinator));
        combined.insert(combined.end(), parts.begin(), parts.end());
        return Selector{combined};
    }

    // Replace every `&` with the parent's parts, tracking the index shift.
    ptrdiff_t offset = 0;
    for (size_t index : relativeIndices) {
        const size_t i = static_cast<size_t>(static_cast<ptrdiff_t>(index) + offset);
        parts.erase(parts.begin() + i);
        parts.insert(parts.begin() + i, parent.parts().begin(), parent.parts().end());
        offset += static_cast<ptrdiff_t>(parent.parts().size()) - 1;
    }

    return Selector{parts};
}

} // namespace

// --- Rules --------------------------------------------------------------------------

namespace
{

struct ParsedDeclaration {
    std::string name;
    std::vector<Value> values;
};

struct ParsedRule {
    std::vector<Selector> selectors;
    std::vector<ParsedDeclaration> declarations;
    std::vector<ParsedRule> nestedRules;
};

struct SheetContext {
    ParsedSheet *sheet;
    std::set<std::filesystem::path> *visited;
    std::filesystem::path basePath; // The file this sheet is parsed from, used for url() resolution.

    void record(ParseError error) const
    {
        if (error.file.empty()) {
            error.file = basePath.string();
        }
        sheet->errors.push_back(Error{
            .file = error.file,
            .line = error.line,
            .column = error.column,
            .message = error.toString(),
        });
    }
};

void consumeImportant(Parser &parser)
{
    if (!parser.consumeDelim('!')) {
        return;
    }
    if (asciiLower(parser.peek().text) == "important") {
        parser.next();
    }
}

// Parses `name: value...` into a declaration, or records an error and recovers.
// Returns nullopt when the declaration produced no property or failed.
std::optional<ParsedDeclaration> parseDeclaration(Parser &parser, SheetContext &context)
{
    auto name = parser.expectIdent();
    if (!name.has_value()) {
        context.record(parser.makeError(ParseError::Kind::UnexpectedToken,
                                        std::format("Invalid declaration: expected a property name, got {}", parser.peek().toString())));
        parser.skipUntilDelim(';');
        return std::nullopt;
    }

    if (!parser.consumeDelim(':')) {
        context.record(parser.makeError(ParseError::Kind::InvalidPropertyValue, std::format("Expected ':' after property name {}", *name)));
        parser.skipUntilDelim(';');
        return std::nullopt;
    }

    const std::string propertyName = *name;
    auto definition = propertyDefinition(propertyName);

    if (definition == nullptr) {
        if (propertyName.starts_with("--")) {
            // Custom properties are stored as definitions with a universal
            // syntax, so var() can resolve them later on.
            auto values = parseValues(universalSyntax(), parser);
            if (!values.has_value()) {
                context.record(parser.ownedError(values.error()));
                parser.skipUntilDelim(';');
                return std::nullopt;
            }

            auto customDefinition = std::make_shared<PropertyDefinition>();
            customDefinition->name = propertyName;
            customDefinition->syntax = std::make_shared<const ParsedPropertySyntax>(universalSyntax());
            customDefinition->initial = values->values;
            addPropertyDefinition(customDefinition);
            return std::nullopt;
        }

        context.record(parser.makeError(ParseError::Kind::UnknownProperty, std::format("No definition for property {}", propertyName)));
        parser.skipUntilDelim(';');
        return std::nullopt;
    }

    auto parsed = parseValues(*definition->syntax, parser);
    if (!parsed.has_value()) {
        context.record(parser.makeError(ParseError::Kind::InvalidPropertyValue,
                                        std::format("Parsing values for property {} failed: {}", propertyName, parsed.error().message)));
        parser.skipUntilDelim(';');
        return std::nullopt;
    }

    consumeImportant(parser);

    return ParsedDeclaration{propertyName, std::move(parsed->values)};
}

// `@import <url>;` at the top level: parse the imported file in place.
void handleImport(Parser &parser, SheetContext &context)
{
    auto url = parser.expectUrlOrString();
    if (!url.has_value()) {
        context.record(parser.makeError(ParseError::Kind::InvalidAtRule, std::format("Invalid @import prelude: got {}", parser.peek().toString())));
        parser.skipUntilDelim(';');
        return;
    }

    if (parser.checkDelim('{')) {
        context.record(parser.makeError(ParseError::Kind::InvalidAtRule, "@import cannot have a block"));
        parser.skipBlock();
        return;
    }
    parser.consumeDelim(';');

    std::filesystem::path importPath(*url);
    if (!importPath.is_absolute()) {
        importPath = context.basePath.parent_path() / importPath;
    }
    importPath = importPath.lexically_normal();

    if (context.visited->contains(importPath)) {
        return; // Import cycle or duplicate; skip silently.
    }

    std::ifstream file(importPath);
    if (!file.is_open()) {
        context.sheet->errors.push_back(Error{
            .file = importPath.string(),
            .line = 0,
            .column = 0,
            .message = std::format("IO Error: could not open \"{}\"", importPath.string()),
        });
        return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    context.visited->insert(importPath);
    auto imported = parseSheetText(buffer.str(), importPath, *context.visited);

    context.sheet->rules.insert(context.sheet->rules.end(), std::make_move_iterator(imported.rules.begin()), std::make_move_iterator(imported.rules.end()));
    context.sheet->errors.insert(context.sheet->errors.end(), std::make_move_iterator(imported.errors.begin()), std::make_move_iterator(imported.errors.end()));
    context.sheet->paths.insert(context.sheet->paths.end(), std::make_move_iterator(imported.paths.begin()), std::make_move_iterator(imported.paths.end()));
}

// `@property <name> { syntax: "..."; inherits: true|false; initial-value: ...; }`
void parsePropertyAtRule(Parser &parser, SheetContext &context)
{
    auto name = parser.expectIdent();
    if (!name.has_value()) {
        context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, "Expected a property name after @property"));
        parser.skipUntilDelim(';');
        return;
    }

    if (!parser.consumeDelim('{')) {
        context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, std::format("Expected a block for @property {}", *name)));
        parser.skipUntilDelim(';');
        return;
    }

    auto definition = std::make_shared<PropertyDefinition>();
    definition->name = *name;
    bool failed = false;

    auto skipToEndOfBlock = [&parser]() {
        while (!parser.atEnd() && !parser.checkDelim('}')) {
            parser.next();
        }
        parser.consumeDelim('}');
    };

    while (!parser.atEnd()) {
        if (parser.consumeDelim(';')) {
            continue;
        }
        if (parser.checkDelim('}')) {
            parser.next();
            break;
        }

        auto key = parser.expectIdent();
        if (!key.has_value()) {
            context.record(parser.ownedError(key.error()));
            parser.skipUntilDelim(';');
            continue;
        }

        if (!parser.consumeDelim(':')) {
            context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, std::format("Expected ':' after {}", *key)));
            parser.skipUntilDelim(';');
            continue;
        }

        const std::string keyName = asciiLower(*key);

        if (keyName == "syntax") {
            std::string syntaxText;
            if (parser.peek().type == Token::Type::QuotedString) {
                syntaxText = parser.next().text;
            } else if (parser.peek().type == Token::Type::Function && asciiLower(parser.peek().text) == "var") {
                // `syntax: var(--other-property)` resolves through the var()
                // function to a string value, like in the CSS var() spec.
                auto function = propertyFunction("var");
                auto result = parser.parseFunctionCall([&function](Parser &p) -> std::expected<std::vector<Value>, ParseError> {
                    return (*function)(p);
                });
                if (!result.has_value()) {
                    context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition,
                                                    std::format("Could not resolve syntax var(): {}", result.error().message)));
                    failed = true;
                    break;
                }
                if (result->size() != 1 || (*result)[0].type() != Value::Type::String) {
                    context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, "Expected a string value for property syntax"));
                    failed = true;
                    break;
                }
                syntaxText = (*result)[0].get<std::string>();
            } else {
                syntaxText = "*";
            }

            auto parsedSyntax = parseSyntaxText(syntaxText, ParseError{});
            if (!parsedSyntax.has_value()) {
                context.record(parser.ownedError(parsedSyntax.error()));
                failed = true;
                break;
            }
            definition->syntax = std::make_shared<const ParsedPropertySyntax>(std::move(*parsedSyntax));
        } else if (keyName == "inherits") {
            auto value = parser.expectIdent();
            if (!value.has_value()) {
                context.record(parser.ownedError(value.error()));
                failed = true;
                break;
            }
            const std::string valueName = asciiLower(*value);
            if (valueName == "true") {
                definition->inherit = true;
            } else if (valueName == "false") {
                definition->inherit = false;
            } else {
                context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, "Unexpected value for inherit"));
                failed = true;
                break;
            }
        } else if (keyName == "initial-value") {
            if (!definition->syntax) {
                context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, "'syntax' is required before 'initial-value'"));
                failed = true;
                break;
            }
            auto values = parseValues(*definition->syntax, parser);
            if (!values.has_value()) {
                context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, std::format("Invalid initial-value: {}", values.error().message)));
                failed = true;
                break;
            }
            definition->initial = std::move(values->values);
        } else {
            // Unknown keys inside @property are ignored, like in the previous parser.
            parser.skipUntilDelim(';');
        }
    }

    if (failed) {
        skipToEndOfBlock();
        return;
    }

    if (!definition->syntax) {
        context.record(parser.makeError(ParseError::Kind::InvalidPropertyDefinition, "'syntax' is required for property definitions"));
        skipToEndOfBlock();
        return;
    }

    addPropertyDefinition(definition); // The first definition of a property wins.
}

void handleAtRule(Parser &parser, SheetContext &context, bool topLevel);

// Parses the prelude and block of a qualified rule (a selector followed by `{}`).
// `nested` rules live inside another rule's block; their selectors may start
// with a combinator (`> direct`), relative to the parent selector.
ParsedRule parseQualifiedRule(Parser &parser, SheetContext &context, bool nested)
{
    ParsedRule rule;

    auto selectors = parseSelectorList(parser, /*allowRelative=*/nested);
    if (!selectors.has_value()) {
        ParseError error = parser.ownedError(selectors.error());
        if (error.kind == ParseError::Kind::InvalidSelectors) {
            error.message = std::format("Invalid Selectors: {}", error.message);
        }
        context.record(error);

        // Recovery: skip up to and including the whole block.
        while (!parser.atEnd() && !parser.checkDelim('{')) {
            parser.next();
        }
        parser.skipBlock();
        return rule;
    }

    rule.selectors = std::move(*selectors);

    if (!parser.consumeDelim('{')) {
        context.record(parser.makeError(ParseError::Kind::InvalidQualifiedRule, "Expected a block after the selector"));
        parser.skipUntilDelim(';');
        return rule;
    }

    bool closedBlock = false;
    while (!parser.atEnd()) {
        if (parser.consumeDelim(';')) {
            continue;
        }

        if (parser.checkDelim('}')) {
            parser.next();
            closedBlock = true;
            break;
        }

        if (parser.check(Token::Type::AtKeyword)) {
            handleAtRule(parser, context, false);
            continue;
        }

        if (parser.seesBlockStart()) {
            rule.nestedRules.push_back(parseQualifiedRule(parser, context, /*nested=*/true));
            continue;
        }

        if (auto declaration = parseDeclaration(parser, context)) {
            rule.declarations.push_back(std::move(*declaration));
        }
    }

    if (!closedBlock) {
        context.record(parser.makeError(ParseError::Kind::UnexpectedEndOfInput, "Unexpected end of input: expected '}' to close the rule block"));
    }

    return rule;
}

// Dispatches an at-rule to its handler.
void handleAtRule(Parser &parser, SheetContext &context, bool topLevel)
{
    const std::string name = parser.peek().text;
    const std::string lowerName = asciiLower(name);
    parser.next(); // Consume the at-keyword itself.

    if (lowerName == "import") {
        if (!topLevel) {
            context.record(parser.makeError(ParseError::Kind::UnsupportedAtRule, "@import can only be used at top level"));
            parser.skipUntilDelim(';');
            return;
        }
        handleImport(parser, context);
        return;
    }

    if (lowerName == "property") {
        parsePropertyAtRule(parser, context);
        return;
    }

    context.record(parser.makeError(ParseError::Kind::UnsupportedAtRule, std::format("Unsupported @-rule @{}", name)));

    // Skip the prelude and the block (if present) so parsing can continue.
    while (!parser.atEnd() && !parser.checkDelim('{') && !parser.checkDelim(';')) {
        parser.next();
    }
    if (parser.checkDelim('{')) {
        parser.skipBlock();
    } else {
        parser.consumeDelim(';');
    }
}

// Flattens a parsed rule tree into a list of rules with fully resolved
// selectors, resolving url() values against the sheet's location.
std::vector<Rule> flattenRule(const ParsedRule &rule, const Selector *parent, const std::filesystem::path &basePath)
{
    std::vector<Rule> result;

    for (const auto &selector : rule.selectors) {
        Selector effective = parent != nullptr ? combineSelectors(selector, *parent) : selector;

        if (effective.parts().empty() && rule.declarations.empty()) {
            continue;
        }

        std::vector<Property> properties;
        properties.reserve(rule.declarations.size());
        for (const auto &declaration : rule.declarations) {
            std::vector<Value> values = declaration.values;
            for (auto &value : values) {
                if (value.type() == Value::Type::Url) {
                    std::filesystem::path urlPath(value.get<std::string>());
                    if (!urlPath.is_absolute()) {
                        urlPath = (basePath.parent_path() / urlPath).lexically_normal();
                    }
                    value = makeUrlValue(urlPath.string());
                }
            }
            properties.emplace_back(declaration.name, std::move(values));
        }

        result.emplace_back(effective, properties);

        for (const auto &nested : rule.nestedRules) {
            auto nestedRules = flattenRule(nested, &effective, basePath);
            result.insert(result.end(), std::make_move_iterator(nestedRules.begin()), std::make_move_iterator(nestedRules.end()));
        }
    }

    return result;
}

} // namespace

ParsedSheet parseSheetText(const std::string &text, const std::filesystem::path &sourcePath, std::set<std::filesystem::path> &visited)
{
    ParsedSheet sheet;
    sheet.paths.push_back(sourcePath);
    visited.insert(sourcePath);

    Parser parser(tokenize(text), sourcePath.empty() ? std::string("<string>") : sourcePath.string());
    SheetContext context{.sheet = &sheet, .visited = &visited, .basePath = sourcePath};

    while (!parser.atEnd()) {
        const size_t before = parser.mark();

        if (parser.consumeDelim(';')) {
            continue;
        }
        if (parser.atEnd()) {
            break;
        }

        if (parser.check(Token::Type::AtKeyword)) {
            handleAtRule(parser, context, /*topLevel=*/true);
            continue;
        }

        ParsedRule rule = parseQualifiedRule(parser, context, /*nested=*/false);
        auto rules = flattenRule(rule, /*parent=*/nullptr, sourcePath);
        sheet.rules.insert(sheet.rules.end(), std::make_move_iterator(rules.begin()), std::make_move_iterator(rules.end()));

        if (parser.mark() == before) {
            parser.next(); // Guarantee progress on unexpected tokens.
        }
    }

    return sheet;
}

ParsedSheet parseSheetFile(const std::filesystem::path &path, std::set<std::filesystem::path> &visited)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        ParsedSheet sheet;
        sheet.paths.push_back(path);
        sheet.errors.push_back(Error{
            .file = path.string(),
            .line = 0,
            .column = 0,
            .message = std::format("IO Error: could not open \"{}\"", path.string()),
        });
        return sheet;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();

    return parseSheetText(buffer.str(), path, visited);
}

}
}
