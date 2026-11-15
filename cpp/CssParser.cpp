// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2024 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "CssParser.h"

#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Parser.h"

namespace cssparser
{

Property::Property(const std::string &name, const std::vector<Value> &values)
    : m_name(name)
    , m_values(values)
{
}

Rule::Rule()
{
}

Rule::Rule(const Selector &selector, const std::vector<Property> &properties)
    : m_selector(selector)
    , m_properties(properties)
{
}

struct StyleSheet::Private {
    explicit Private(std::filesystem::path path)
        : path(std::move(path))
    {
    }

    /*!
     * Parse \a text as a stylesheet that originated from the file \a sourcePath
     * and append the resulting rules, errors and paths to this sheet.
     */
    void parseText(const std::string &text, const std::filesystem::path &sourcePath);
    /*!
     * Read \a filePath from disk, parse it and append the results. A missing or
     * unreadable file is reported as an error, never thrown.
     */
    void parseFile(const std::filesystem::path &filePath);

    std::filesystem::path path;

    std::vector<Rule> rules;
    std::vector<Error> errors;
    std::vector<std::filesystem::path> paths;
};

void StyleSheet::Private::parseText(const std::string &text, const std::filesystem::path &sourcePath)
{
    std::set<std::filesystem::path> visited;
    auto sheet = detail::parseSheetText(text, sourcePath, visited);

    paths.insert(paths.end(), std::make_move_iterator(sheet.paths.begin()), std::make_move_iterator(sheet.paths.end()));
    rules.insert(rules.end(), std::make_move_iterator(sheet.rules.begin()), std::make_move_iterator(sheet.rules.end()));
    errors.insert(errors.end(), std::make_move_iterator(sheet.errors.begin()), std::make_move_iterator(sheet.errors.end()));
}

void StyleSheet::Private::parseFile(const std::filesystem::path &filePath)
{
    std::set<std::filesystem::path> visited;
    auto sheet = detail::parseSheetFile(filePath, visited);

    paths.insert(paths.end(), std::make_move_iterator(sheet.paths.begin()), std::make_move_iterator(sheet.paths.end()));
    rules.insert(rules.end(), std::make_move_iterator(sheet.rules.begin()), std::make_move_iterator(sheet.rules.end()));
    errors.insert(errors.end(), std::make_move_iterator(sheet.errors.begin()), std::make_move_iterator(sheet.errors.end()));
}

StyleSheet::StyleSheet(const std::filesystem::path &path)
    : d(std::make_unique<Private>(path))
{
}

StyleSheet::~StyleSheet() = default;

std::span<const Rule> StyleSheet::rules() const
{
    return std::span<const Rule>(d->rules.cbegin(), d->rules.cend());
}

std::span<const Error> StyleSheet::errors() const
{
    return std::span<const Error>(d->errors.cbegin(), d->errors.cend());
}

std::span<const std::filesystem::path> StyleSheet::paths() const
{
    return std::span<const std::filesystem::path>(d->paths.cbegin(), d->paths.cend());
}

void StyleSheet::parse()
{
    d->parseFile(d->path);
}

void StyleSheet::parseString(const std::string &source)
{
    // The path of this sheet is used as the origin for error reporting and for
    // resolving relative url() values.
    d->parseText(source, d->path);
}

void StyleSheet::import(const std::filesystem::path &path)
{
    std::filesystem::path resolved = path;
    if (!resolved.is_absolute()) {
        auto parent = d->path.parent_path();
        if (parent.empty()) {
            // No base directory to resolve against; use the path as given.
            resolved = path;
        } else {
            resolved = parent / path;
        }
    }

    d->parseFile(resolved.lexically_normal());
}

}
