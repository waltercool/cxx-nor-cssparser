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

    if (gFailed > 0) {
        std::cerr << gFailed << " test(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All tests passed" << std::endl;
    return 0;
}
