// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "CssParser.h"
#include "Selector.h"
#include "Tokenizer.h"

namespace cssparser
{
namespace detail
{

/*!
 * An error that happened while parsing CSS.
 *
 * This keeps the kind of error around so the parser can distinguish between
 * recoverable and fatal errors, before converting to the public
 * cssparser::Error struct.
 */
struct ParseError {
    enum class Kind : uint8_t {
        Unspecified,
        Unimplemented,
        UnexpectedEndOfInput,
        Unknown,
        UnknownProperty,
        UnexpectedToken,
        InvalidSelectors,
        InvalidPropertySyntax,
        InvalidPropertyValue,
        UnknownFunction,
        InvalidPropertyDefinition,
        PropertyValueDoesNotMatchSyntax,
        UnsupportedAtRule,
        InvalidAtRule,
        InvalidQualifiedRule,
        FileError,
        StyleSheetParseError,
    };

    Kind kind = Kind::Unspecified;
    std::string message;
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;

    std::string toString() const;
};

/*!
 * A token stream parser with save/restore points, used for backtracking.
 *
 * The whole input is pre-tokenized; all parsing operations work on the
 * resulting vector, which makes backtracking trivial and cheap.
 */
struct DefinitionRegistry;

class Parser
{
public:
    Parser(std::vector<Token> tokens, std::string sourceName);

    const Token &peek(size_t lookahead = 0) const;
    const Token &next();
    bool atEnd() const;

    size_t mark() const;
    void reset(size_t mark);

    bool check(Token::Type type) const;
    bool checkDelim(char c) const;
    bool checkIdent(const std::string &name) const;
    bool consumeDelim(char c);

    std::expected<std::string, ParseError> expectIdent();
    std::expected<double, ParseError> expectNumber();
    std::expected<int, ParseError> expectInteger();
    std::expected<std::string, ParseError> expectString();
    std::expected<std::string, ParseError> expectUrlOrString();
    std::expected<void, ParseError> expectComma();

    /*!
     * Runs \a fn; if it fails, the input is rewound to the current position.
     * Returns std::nullopt when \a fn failed, its value otherwise.
     */
    template<typename F>
    auto tryParse(F &&fn) -> std::optional<typename std::invoke_result_t<F, Parser &>::value_type>
    {
        size_t checkpoint = mark();
        auto result = fn(*this);
        if (!result.has_value()) {
            reset(checkpoint);
            return std::nullopt;
        }
        return {std::move(*result)};
    }

    /*!
     * Parses a `name( ... )` call. The current token must be a Function token.
     *
     * The Function token is consumed, \a fn is run over the contents of the
     * parentheses and the closing `)` is consumed. When \a fn fails, the input
     * is skipped up to and including the matching `)` so parsing can continue.
     */
    template<typename F>
    auto parseFunctionCall(F &&fn) -> std::expected<typename std::invoke_result_t<F, Parser &>::value_type, ParseError>
    {
        using T = typename std::invoke_result_t<F, Parser &>::value_type;

        std::string name = peek().text;
        next(); // Consume the Function token.

        size_t contentStart = mark();
        auto result = fn(*this);

        if (result.has_value()) {
            if (atEnd() || !checkDelim(')')) {
                return std::unexpected(makeError(ParseError::Kind::UnexpectedToken, "Expected a closing parenthesis for function " + name));
            }
            next();
            return {std::move(*result)};
        }

        // The function failed; skip past the matching closing parenthesis so
        // callers can continue, and propagate the error.
        ParseError error = std::move(result.error());
        if (error.file.empty()) {
            error.file = m_sourceName;
        }
        reset(contentStart);
        int depth = 1;
        while (!atEnd()) {
            if (check(Token::Type::Function) || checkDelim('(')) {
                depth++;
            } else if (checkDelim(')')) {
                depth--;
                if (depth == 0) {
                    next();
                    break;
                }
            }
            next();
        }
        return std::unexpected(std::move(error));
    }

    // Skips a balanced { ... } block starting at the current token.
    void skipBlock();
    // Skips forward, consuming tokens until (and including) the next \a c that
    // is not inside a { ... } or ( ... ) block, or until end of input.
    void skipUntilDelim(char c);

    // Whether a { appears before any ; or } at block depth 0 from here, which
    // is used to decide whether what comes next is a declaration or a rule.
    bool seesBlockStart() const;

    ParseError makeError(ParseError::Kind kind, const std::string &message) const;
    // Copies \a other, filling in this parser's source name if the error has none.
    ParseError ownedError(const ParseError &other) const;

    const std::string &sourceName() const;

    // The property definition registry this parser resolves definitions and
    // var() references against. Owned by the parse state of the stylesheet
    // being parsed; set before parsing begins. When null, every lookup misses.
    DefinitionRegistry *registry = nullptr;

private:
    std::vector<Token> m_tokens;
    size_t m_pos = 0;
    std::string m_sourceName;

    static const Token s_endToken;
};

using PropertyFunctionResult = std::expected<std::vector<Value>, ParseError>;
using PropertyFunction = std::function<PropertyFunctionResult(Parser &)>;

// Looks up a registered property function, such as var or mix. The registry is
// pre-populated with the built-in functions var, mix, custom-color and
// modify-color; additional functions can be registered process-wide.
std::optional<PropertyFunction> propertyFunction(std::string_view name);
bool addPropertyFunction(std::string_view name, PropertyFunction function);

struct PropertyDefinition;

/*!
 * A registry of custom property definitions (`@property` rules and
 * `--custom-property: value` declarations). *
 * Each stylesheet owns one registry (see SheetParseState). Definitions are
 * scoped to the stylesheet they belong to and the files it imports; they do
 * not leak between independent stylesheets. A registry is used from whichever
 * thread parses its stylesheet, so concurrent parsing of the same sheet is
 * the caller's responsibility.
 */
struct DefinitionRegistry {
    /*!
     * Returns the definition of \a name, or nullptr when this sheet has no
     * definition for it.
     */
    std::shared_ptr<const PropertyDefinition> find(const std::string &name) const;

    /*!
     * Registers \a definition. The first definition of a property wins; later
     * definitions of the same name are ignored.
     * Returns true when the definition was added.
     */
    bool add(std::shared_ptr<PropertyDefinition> definition);

    /*!
     * Declares a custom property `--name` with the given values, as produced by
     * a `--name: value` declaration. Updates the value of an existing
     * custom-property declaration in place, so later var() references resolve
     * to the most recently declared value.
     */
    void declareCustom(const std::string &name, std::vector<Value> values);

    std::vector<std::shared_ptr<PropertyDefinition>> definitions;
};

struct PropertyDefinition {
    std::string name;
    std::shared_ptr<const struct ParsedPropertySyntax> syntax;
    bool inherit = false;
    std::vector<Value> initial;
    bool custom = false; //< True when created by a `--name: value` declaration instead of `@property`.
};

/*!
 * State that is shared across all parse operations of one StyleSheet: the set
 * of files already imported (guarding against import cycles and duplicates)
 * and the stylesheet's property definition registry.
 */
struct SheetParseState {
    std::set<std::filesystem::path> visited;
    DefinitionRegistry registry;
};

/*!
 * The result of parsing one stylesheet: the flattened list of rules, all
 * errors that were encountered and the files that were parsed.
 */
struct ParsedSheet {
    std::vector<Rule> rules;
    std::vector<Error> errors;
    std::vector<std::filesystem::path> paths;
};

/*!
 * Parse a full stylesheet from \a text.
 *
 * The parser follows the structure of the CSS grammar: qualified rules with
 * declarations and nested rules, `@property` definitions, and top level
 * `@import` statements, which are recursively parsed relative to the directory
 * of \a sourcePath. \a state carries the per-stylesheet parse state (visited
 * files and property definitions); pass a fresh SheetParseState for each
 * independent stylesheet, or the same one to all parses that belong to one
 * stylesheet so definitions and imports stay shared between them.
 */
ParsedSheet parseSheetText(const std::string &text, const std::filesystem::path &sourcePath, SheetParseState &state);

/*!
 * Read the file at \a path and parse it as a stylesheet.
 *
 * On IO failure an Error with kind "IO Error" is recorded instead of throwing.
 */
ParsedSheet parseSheetFile(const std::filesystem::path &path, SheetParseState &state);
}
}
