/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSRGB.h"
#include <AK/TypeCasts.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>

namespace Web::CSS {

Color CSSRGB::to_color(Optional<Layout::NodeWithStyle const&>, CalculationResolutionContext const& resolution_context) const
{
    auto resolve_rgb_to_u8 = [&resolution_context](CSSStyleValue const& style_value) -> Optional<u8> {
        // <number> | <percentage> | none
        auto normalized = [](double number) {
            if (isnan(number))
                number = 0;
            return llround(clamp(number, 0.0, 255.0));
        };

        if (style_value.is_number())
            return normalized(style_value.as_number().number());

        if (style_value.is_percentage())
            return normalized(style_value.as_percentage().value() * 255 / 100);

        if (style_value.is_calculated()) {
            auto const& calculated = style_value.as_calculated();
            if (calculated.resolves_to_number())
                return normalized(calculated.resolve_number(resolution_context).value());
            if (calculated.resolves_to_percentage())
                return normalized(calculated.resolve_percentage(resolution_context).value().value() * 255 / 100);
        }

        if (style_value.is_keyword() && style_value.to_keyword() == Keyword::None)
            return 0;

        return {};
    };

    auto resolve_alpha_to_u8 = [&resolution_context](CSSStyleValue const& style_value) -> Optional<u8> {
        auto alpha_0_1 = resolve_alpha(style_value, resolution_context);
        if (alpha_0_1.has_value())
            return llround(clamp(alpha_0_1.value() * 255.0f, 0.0f, 255.0f));
        return {};
    };

    u8 const r_val = resolve_rgb_to_u8(m_properties.r).value_or(0);
    u8 const g_val = resolve_rgb_to_u8(m_properties.g).value_or(0);
    u8 const b_val = resolve_rgb_to_u8(m_properties.b).value_or(0);
    u8 const alpha_val = resolve_alpha_to_u8(m_properties.alpha).value_or(255);

    return Color(r_val, g_val, b_val, alpha_val);
}

bool CSSRGB::equals(CSSStyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_color = other.as_color();
    if (color_type() != other_color.color_type())
        return false;
    auto const& other_rgb = static_cast<CSSRGB const&>(other_color);
    return m_properties == other_rgb.m_properties;
}

// https://www.w3.org/TR/css-color-4/#serializing-sRGB-values
String CSSRGB::to_string(SerializationMode mode) const
{
    // FIXME: Do this properly, taking unresolved calculated values into account.
    if (mode != SerializationMode::ResolvedValue && m_properties.name.has_value())
        return m_properties.name.value().to_string().to_ascii_lowercase();
    return serialize_a_srgb_value(to_color({}, {}));
}

}
