// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "Selector.h"

#include <format>

using namespace std::string_literals;

namespace cssparser
{

AttributeMatch::AttributeMatch(const std::string &name, Operator op, const Value &value)
    : m_name(name)
    , m_operator(op)
    , m_value(value)
{
}

namespace
{
std::string kindToString(SelectorPart::Kind kind)
{
    switch (kind) {
    case SelectorPart::Kind::Unknown:
        return "Unknown"s;
    case SelectorPart::Kind::AnyElement:
        return "AnyElement"s;
    case SelectorPart::Kind::Type:
        return "Type"s;
    case SelectorPart::Kind::Class:
        return "Class"s;
    case SelectorPart::Kind::Id:
        return "Id"s;
    case SelectorPart::Kind::Attribute:
        return "Attribute"s;
    case SelectorPart::Kind::RelativeParent:
        return "RelativeParent"s;
    case SelectorPart::Kind::PseudoClass:
        return "PseudoClass"s;
    case SelectorPart::Kind::DocumentRoot:
        return "DocumentRoot"s;
    case SelectorPart::Kind::Part:
        return "Part"s;
    case SelectorPart::Kind::DescendantCombinator:
        return "DescendantCombinator"s;
    case SelectorPart::Kind::ChildCombinator:
        return "ChildCombinator"s;
    case SelectorPart::Kind::CombinatorStart:
        return "CombinatorStart"s;
    case SelectorPart::Kind::PartCombinator:
        return "PartCombinator"s;
    }

    return "Unknown"s;
}
} // namespace

SelectorPart::SelectorPart()
{
}

SelectorPart::SelectorPart(Kind kind, const Value &value, std::optional<AttributeMatch> match)
    : m_kind(kind)
    , m_value(value)
    , m_attributeMatch(match)
{
}

std::string SelectorPart::toString() const
{
    auto kind = kindToString(m_kind);
    switch (m_kind) {
    case SelectorPart::Kind::Unknown:
    case SelectorPart::Kind::AnyElement:
    case SelectorPart::Kind::DocumentRoot:
    case SelectorPart::Kind::DescendantCombinator:
    case SelectorPart::Kind::ChildCombinator:
    case SelectorPart::Kind::PartCombinator:
        return std::format("SelectorPart(type: {})", kind);
    default:
        return std::format("SelectorPart(type: {}, value: {})", kind, m_value.toString());
    }
}

Selector::Selector()
{
}

Selector::Selector(const std::vector<SelectorPart> &parts)
    : m_parts(parts)
{
}

std::string Selector::toString() const
{
    std::string parts;

    for (const auto &part : m_parts) {
        if (!parts.empty()) {
            parts += " "s;
        }

        parts += part.toString();
    }

    return std::format("Selector(parts: {})", parts);
}

}
