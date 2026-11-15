// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
// SPDX-FileCopyrightText: 2026 Arjen Hiemstra <ahiemstra@heimr.nl>

#include "Color.h"

#include <format>

using namespace std::string_literals;

namespace cssparser
{
namespace Color
{

RgbaData::RgbaData()
{
}

RgbaData::RgbaData(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    : m_r(r)
    , m_g(g)
    , m_b(b)
    , m_a(a)
{
}

std::string RgbaData::toString() const
{
    return std::format("RGBA({}, {}, {}, {})", m_r, m_g, m_b, m_a);
}

CustomColorData::CustomColorData()
{
}

CustomColorData::CustomColorData(const std::string &source, const std::vector<std::string> &args)
    : m_source(source)
    , m_arguments(args)
{
}

std::string CustomColorData::toString() const
{
    auto args = std::string{};

    for (const auto &arg : m_arguments) {
        if (!args.empty()) {
            args += ", ";
        }
        args += arg;
    }

    return std::vformat("CustomColor(source: {}, arguments: {})"s, std::make_format_args(m_source, args));
}

MixOperationData::MixOperationData()
{
}

MixOperationData::MixOperationData(const std::shared_ptr<Color> &other, float amount)
    : m_other(other)
    , m_amount(amount)
{
}

std::string MixOperationData::toString() const
{
    return std::format("MixOperationData(other: {}, amount: {})", m_other ? m_other->toString() : "None", m_amount);
}

SetOperationData::SetOperationData()
{
}

SetOperationData::SetOperationData(std::optional<uint8_t> r, std::optional<uint8_t> g, std::optional<uint8_t> b, std::optional<uint8_t> a)
    : m_r(r)
    , m_g(g)
    , m_b(b)
    , m_a(a)
{
}

std::string SetOperationData::toString() const
{
    return std::format("SetOperationData(r: {}, g: {}, b: {}, a: {})", m_r.value_or(-1), m_g.value_or(-1), m_b.value_or(-1), m_a.value_or(-1));
}

ModifiedColorData::ModifiedColorData()
{
}

std::string ModifiedColorData::toString() const
{
    const auto color = m_color ? m_color->toString() : "None"s;

    switch (m_operation) {
    case Operation::Add: {
        const auto add = std::get<std::shared_ptr<Color>>(m_data)->toString();
        return std::vformat("ModifiedColor(color: {}, operation: add, data: {})"s, std::make_format_args(color, add));
    }
    case Operation::Subtract: {
        const auto sub = std::get<std::shared_ptr<Color>>(m_data)->toString();
        return std::vformat("ModifiedColor(color: {}, operation: subtract, data: {})"s, std::make_format_args(color, sub));
    }
    case Operation::Multiply: {
        const auto mul = std::get<std::shared_ptr<Color>>(m_data)->toString();
        return std::vformat("ModifiedColor(color: {}, operation: multiply, data: {})"s, std::make_format_args(color, mul));
    }
    case Operation::Set: {
        const auto data = std::get<SetOperationData>(m_data).toString();
        return std::vformat("ModifiedColor(color: {}, operation: set, data: {})"s, std::make_format_args(color, data));
    }
    case Operation::Mix: {
        const auto data = std::get<MixOperationData>(m_data).toString();
        return std::vformat("ModifiedColor(color: {}, operation: mix, data: {})"s, std::make_format_args(color, data));
    }
    default:
        return "ModifiedColor(unknown operation)"s;
    }
}

Color::Color()
{
}

std::string Color::toString() const
{
    switch (m_type) {
    case Type::Empty:
        return "Color(Empty)"s;
    case Type::Rgba:
        return std::get<RgbaData>(m_data).toString();
    case Type::Custom:
        return std::get<CustomColorData>(m_data).toString();
    case Type::Modified:
        return std::get<ModifiedColorData>(m_data).toString();
    default:
        return "Color(Unknown)"s;
    }
}

}
}
