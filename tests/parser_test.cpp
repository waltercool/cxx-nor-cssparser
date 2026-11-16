// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

// Behaviour tests for the pure C++ parser. These cover the observable behaviours
// that were previously provided by the Rust implementation (see the removed
// rust/src/tests/*.rs), including error message shapes that consumers rely on.

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#include "CssParser.h"

using namespace cssparser;

namespace
{

int gFailed = 0;

void check(bool condition, const std::string &what)
{
    if (!condition) {
        gFailed++;
        std::cerr << "FAIL: " << what << std::endl;
    }
}

std::filesystem::path dataPath(const char *file)
{
    return std::filesystem::path(TEST_DATA_DIR) / file;
}

// Parses a data file and applies the check function to the resulting sheet.
void parseFile(const char *file, const std::function<void(const StyleSheet &)> &checkSheet)
{
    StyleSheet sheet(dataPath(file));
    sheet.parse();
    checkSheet(sheet);
}

// Parses an inline sheet from a string and applies the check function.
void parseInline(const std::string &data, const std::function<void(const StyleSheet &)> &checkSheet)
{
    StyleSheet sheet("<test>");
    sheet.parseString(data);
    checkSheet(sheet);
}

bool hasErrorContaining(const StyleSheet &sheet, std::string_view needle)
{
    for (const auto &error : sheet.errors()) {
        if (error.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const Rule *findRuleWithProperty(const StyleSheet &sheet, const std::string &selectorNeedle, const std::string &propertyName)
{
    for (const auto &rule : sheet.rules()) {
        if (rule.selector().toString().find(selectorNeedle) == std::string::npos) {
            continue;
        }
        for (const auto &property : rule.properties()) {
            if (property.name() == propertyName) {
                return &rule;
            }
        }
    }
    return nullptr;
}

// --- Test cases ------------------------------------------------------------------

void testComplexFile()
{
    parseFile("complex.css", [](const StyleSheet &sheet) {
        check(sheet.errors().empty(), "complex.css should parse without errors");
        check(!sheet.rules().empty(), "complex.css should produce rules");
    });
}

void testImportFile()
{
    // import.css @imports definitions and values; everything should resolve.
    parseFile("import.css", [](const StyleSheet &sheet) {
        check(sheet.errors().empty(), "import.css should parse without errors");
        check(sheet.paths().size() >= 3, "import.css should track imported files in paths()");
        check(!sheet.rules().empty(), "import.css should produce rules");
    });
}

void testImportErrorsFile()
{
    // import_errors.css imports a file that deliberately contains errors; the
    // errors must be reported with the file they occurred in.
    parseFile("import_errors.css", [](const StyleSheet &sheet) {
        check(sheet.errors().size() == 3, "import_errors.css should report exactly three errors");
        check(hasErrorContaining(sheet, "No definition for property unknown-property"), "import_errors.css should report unknown property error");
        check(hasErrorContaining(sheet, "Invalid Selectors"), "import_errors.css should report invalid selector error");
        check(hasErrorContaining(sheet, "Parsing values for property padding-top failed"), "import_errors.css should report invalid property value error");
    });
}

void testFunctionsFile()
{
    // Covers var(), mix(), custom-color(), modify-color() with all operations,
    // currentcolor, :root, ::part() and a relative nested rule.
    parseFile("functions.css", [](const StyleSheet &sheet) {
        check(sheet.errors().empty(), "functions.css should parse without errors");
        check(!sheet.rules().empty(), "functions.css should produce rules");

        // Spot-check that var() substituted the known property's initial value.
        const Rule *box = findRuleWithProperty(sheet, "box", "width");
        check(box != nullptr, "box rule with width should exist");
        if (box) {
            for (const auto &property : box->properties()) {
                if (property.name() != "width") {
                    continue;
                }
                const auto text = property.value(0).toString();
                // `width: var(--missing, 10px)` keeps the fallback value.
                check(text.find("10") != std::string::npos, "var() fallback should be kept for unknown properties");
            }
        }
    });
}

void testUndefinedProperty()
{
    parseInline(R"(
        box { unknown-property: 5; }
    )",
                [](const StyleSheet &sheet) {
                    check(sheet.errors().size() == 1, "unknown property should produce exactly one error");
                    check(hasErrorContaining(sheet, "No definition for property unknown-property"), "unknown property error should name the property");
                });
}

void testImportAtNonTopLevel()
{
    parseInline(R"(
        @property width { syntax: "<length>"; inherits: false; }
        box {
            @import "other.css";
            width: 10px;
        }
    )",
                [](const StyleSheet &sheet) {
                    check(hasErrorContaining(sheet, "@import can only be used at top level"), "@import inside a rule block should be rejected");
                });
}

void testDanglingCombinator()
{
    // At the top level a selector may not start with a combinator...
    parseInline(R"(
        @property color { syntax: "<color>"; inherits: true; initial-value: red; }
        > direct { color: red; }
    )",
                [](const StyleSheet &sheet) {
                    check(hasErrorContaining(sheet, "Dangling combinator"), "top-level leading combinator should be dangling");
                });

    // ...but inside a rule it is a relative selector and must work.
    parseInline(R"(
        @property color { syntax: "<color>"; inherits: true; initial-value: red; }
        outer {
            color: blue;
            > direct { color: green; }
            &.deep { color: yellow; }
        }
    )",
                [](const StyleSheet &sheet) {
                    check(sheet.errors().empty(), "relative nested selectors should parse without errors");

                    const Rule *rule = findRuleWithProperty(sheet, "direct", "color");
                    check(rule != nullptr, "a rule for `> direct` should exist in the flattened output");
                    if (rule) {
                        const auto text = rule->selector().toString();
                        check(text.find("outer") != std::string::npos, "relative selector should be combined with the parent");
                    }
                });
}

void testVarBehaviour()
{
    // `var()` of a defined custom property substitutes its initial value and
    // ignores the fallback; undefined ones use the fallback; undefined ones
    // without a fallback are an error naming the property.
    parseInline(R"(
        @property width { syntax: "<length>"; inherits: false; initial-value: 12px; }
        @property --special { syntax: "<length>"; inherits: false; initial-value: 8px; }
        box { width: var(--width, 99px); }
        other { width: var(--missing); }
        third { width: var(--special, 100px); }
    )",
                [](const StyleSheet &sheet) {
                    check(sheet.errors().size() == 1, "only the fallback-less undefined reference should error");
                    check(hasErrorContaining(sheet, "No custom property --missing was defined"),
                          "var() of an undefined custom property without fallback should report the property name");

                    // The `third` rule must have substituted `--special`'s initial value (8px),
                    // not the fallback (100px).
                    bool checked = false;
                    for (const auto &rule : sheet.rules()) {
                        if (rule.selector().toString().find("third") == std::string::npos) {
                            continue;
                        }
                        for (const auto &property : rule.properties()) {
                            if (property.name() != "width") {
                                continue;
                            }
                            checked = true;
                            const auto text = property.value(0).toString();
                            check(text.find("8") != std::string::npos && text.find("100") == std::string::npos,
                                  "var() of a defined property should substitute its initial value, not the fallback");
                        }
                    }
                    check(checked, "the third rule should define width through var()");
                });
}

// Collect the `width` values (as strings) for rules whose selector mentions
// `selectorNeedle`, so a test can assert on the resolved value.
std::vector<std::string> widthValuesFor(const StyleSheet &sheet, const std::string &selectorNeedle)
{
    std::vector<std::string> result;
    for (const auto &rule : sheet.rules()) {
        if (rule.selector().toString().find(selectorNeedle) == std::string::npos) {
            continue;
        }
        for (const auto &property : rule.properties()) {
            if (property.name() == "width") {
                std::string joined;
                for (const auto &value : property.values()) {
                    joined += value.toString() + " ";
                }
                result.push_back(joined);
            }
        }
    }
    return result;
}

void testPerStylesheetScoping()
{
    // Property definitions are scoped to a stylesheet and its imports; they do
    // not leak between independent stylesheets parsed in the same process.
    // This mirrors the behaviour the Rust implementation gained in
    // "Scope property definitions to each stylesheet and its imports".

    // 1) Two stylesheets with the same `@property width` and their own `--size`
    //    must each resolve `var(--size)` to their own value.
    {
        StyleSheet first("<test>");
        first.parseString(R"(@property width { syntax: "<length>"; }
                             :root { --size: 10px; }
                             button { width: var(--size); })");
        StyleSheet second("<test>");
        second.parseString(R"(@property width { syntax: "<length>"; }
                              :root { --size: 20px; }
                              button { width: var(--size); })");

        auto w1 = widthValuesFor(first, "button");
        check(w1.size() == 1 && w1[0].find("10") != std::string::npos, "first sheet resolves --size to 10px");
        auto w2 = widthValuesFor(second, "button");
        check(w2.size() == 1 && w2[0].find("20") != std::string::npos, "second sheet resolves --size to its own 20px");

        // A re-parse of the first sheet still resolves to 10px (its registry persists).
        first.parseString("button { width: var(--size); }");
        auto w1b = widthValuesFor(first, "button");
        check(w1b.size() == 2 && w1b[1].find("10") != std::string::npos, "re-parse on first sheet stays 10px");
    }

    // 2) Independent stylesheets can define different syntax for the same name.
    {
        // Silence `width` here as <color> rather than inheriting a <length> from
        // an earlier sheet in the same process.
        StyleSheet sheet("<test>");
        sheet.parseString(R"(@property width { syntax: "<color>"; }
                             button { width: blue; })");
        check(sheet.errors().empty(), "independent sheet can redefine width syntax as <color>");
        auto w = widthValuesFor(sheet, "button");
        // Blue is RGBA(0, 0, 255, 255); "blue" would fail <length> validation.
        check(w.size() == 1 && w[0].find("255") != std::string::npos, "width resolves to a color under <color> syntax");
    }

    // 3) A variable defined in another sheet does not suppress a local fallback.
    {
        StyleSheet other("<test>");
        other.parseString(R"(@property width { syntax: "<length>"; }
                             :root { --size: 10px; })");
        StyleSheet sheet("<test>");
        sheet.parseString(R"(@property width { syntax: "<length>"; }
                             button { width: var(--size, 42px); })");
        auto w = widthValuesFor(sheet, "button");
        check(w.size() == 1 && w[0].find("42") != std::string::npos, "local fallback 42px used even though --size exists elsewhere");
    }

    // 4) Imports share definitions with their parent and siblings.
    {
        const std::filesystem::path dir = dataPath("property_definitions");
        StyleSheet sheet(dir / "parent.css");
        sheet.import("api-imported-definitions.css");
        sheet.parse();
        check(sheet.errors().empty(), "import-sharing sheet parses without errors");

        auto nested = widthValuesFor(sheet, "nested");
        check(nested.size() == 1 && nested[0].find("5") != std::string::npos, "nested uses shared --small (5px)");
        auto sibling = widthValuesFor(sheet, "sibling");
        check(sibling.size() == 1 && sibling[0].find("12") != std::string::npos, "sibling uses --from-child (12px)");
        auto button = widthValuesFor(sheet, "button");
        check(button.size() == 1 && button[0].find("12") != std::string::npos, "button uses --from-child (12px)");
    }

    // 5) @property descriptors and nested fallbacks resolve variables locally.
    {
        StyleSheet sheet("<test>");
        sheet.parseString(R"(@property width { syntax: "<length>"; }
                             :root { --size: 10px; }
                             button { width: var(--missing, var(--size)); }
                             other { width: var(--missing, var(--also-missing, 30px)); })");
        check(sheet.errors().empty(), "nested var() fallbacks parse without errors");
        auto b = widthValuesFor(sheet, "button");
        check(b.size() == 1 && b[0].find("10") != std::string::npos, "var(--missing, var(--size)) resolves to 10px");
        auto o = widthValuesFor(sheet, "other");
        check(o.size() == 1 && o[0].find("30") != std::string::npos, "double-nested fallback resolves to 30px");
    }
}

} // namespace

int main()
{
    testComplexFile();
    testImportFile();
    testImportErrorsFile();
    testFunctionsFile();
    testUndefinedProperty();
    testImportAtNonTopLevel();
    testDanglingCombinator();
    testVarBehaviour();
    testPerStylesheetScoping();

    if (gFailed > 0) {
        std::cerr << gFailed << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All tests passed" << std::endl;
    return 0;
}
