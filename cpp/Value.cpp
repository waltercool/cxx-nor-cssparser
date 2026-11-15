// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "Value.h"

#include <format>

using namespace std::string_literals;

namespace cssparser
{

Value::Value()
{
}

Dimension::Dimension()
{
}

Dimension::Dimension(Unit unit, float value)
    : m_unit(unit)
    , m_value(value)
{
}

std::string Dimension::toString() const
{
    switch (m_unit) {
    case Unit::Unknown:
        return "Unknown unit"s;
    case Unit::Unsupported:
        return "Unsupported unit"s;
    case Unit::Number:
        return std::format("{}", m_value);
    case Unit::Px:
        return std::format("{} px", m_value);
    case Unit::Em:
        return std::format("{} em", m_value);
    case Unit::Rem:
        return std::format("{} rem", m_value);
    case Unit::Pt:
        return std::format("{} pt", m_value);
    case Unit::Percent:
        return std::format("{} %", m_value);
    case Unit::Degrees:
        return std::format("{}°", m_value);
    case Unit::Radians:
        return std::format("{} rad", m_value);
    case Unit::Seconds:
        return std::format("{} s", m_value);
    case Unit::Milliseconds:
        return std::format("{} ms", m_value);
    }

    return std::format("{} (Unknown unit)", m_value);
}

namespace
{
std::string_view valueTypeToString(Value::Type type)
{
    switch (type) {
    case Value::Type::Empty:
        return "Empty"s;
    case Value::Type::Dimension:
        return "Dimension"s;
    case Value::Type::String:
        return "String"s;
    case Value::Type::Color:
        return "Color"s;
    case Value::Type::Image:
        return "Image"s;
    case Value::Type::Url:
        return "Url"s;
    case Value::Type::Integer:
        return "Integer"s;
    }

    return "Unknown"s;
}
} // namespace

std::string Value::toString() const
{
    std::string data;
    switch (m_type) {
    case Value::Type::Empty:
        data = "(Empty)";
        break;
    case Value::Type::Dimension:
        data = std::get<Dimension>(m_data).toString();
        break;
    case Value::Type::String:
    case Value::Type::Image:
    case Value::Type::Url:
        data = std::get<std::string>(m_data);
        break;
    case Value::Type::Color:
        data = std::get<Color::Color>(m_data).toString();
        break;
    case Value::Type::Integer:
        data = std::to_string(std::get<int>(m_data));
    }

    return std::format("Value(type: {}, data: {})", valueTypeToString(m_type), data);
}

}
