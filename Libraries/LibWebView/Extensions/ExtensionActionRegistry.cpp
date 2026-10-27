/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibWebView/Extensions/ExtensionActionRegistry.h>

namespace WebView {

static constexpr Gfx::Color default_badge_background_color { 0xd9, 0, 0 };

Gfx::Color default_badge_text_color(Gfx::Color background)
{
    auto against_white = background.contrast_ratio(Gfx::Color::White);
    auto against_black = background.contrast_ratio(Gfx::Color::Black);
    return against_white >= against_black ? Gfx::Color::White : Gfx::Color::Black;
}

static Optional<Gfx::Color> color_from_json(Optional<JsonValue> const& value)
{
    if (!value.has_value() || !value->is_array() || value->as_array().size() != 4)
        return {};
    auto const& array = value->as_array();
    u8 components[4];
    for (size_t i = 0; i < 4; ++i) {
        auto component = array.at(i).get_integer<u8>();
        if (!component.has_value())
            return {};
        components[i] = *component;
    }
    return Gfx::Color { components[0], components[1], components[2], components[3] };
}

static Vector<ExtensionManifest::IconPath> icon_from_json(JsonValue const& value)
{
    Vector<ExtensionManifest::IconPath> icon;
    if (value.is_string()) {
        icon.append({ 0, value.as_string() });
    } else if (value.is_object()) {
        value.as_object().for_each_member([&](String const& size, JsonValue const& path) {
            if (auto pixels = size.to_number<u32>(); pixels.has_value() && path.is_string())
                icon.append({ *pixels, path.as_string() });
        });
        quick_sort(icon, [](auto const& a, auto const& b) { return a.size < b.size; });
    }
    return icon;
}

void ExtensionActionRegistry::set_extension(String id, String name, ExtensionManifest::Action action)
{
    // An extension that reloads starts over from its manifest, but keeps its place among the others.
    if (auto* entry = find(id)) {
        *entry = { move(id), move(name), move(action), {} };
        return;
    }
    m_entries.append({ move(id), move(name), move(action), {} });
}

void ExtensionActionRegistry::remove_extension(StringView id)
{
    m_entries.remove_first_matching([&](auto const& entry) { return entry.id == id; });
}

bool ExtensionActionRegistry::has_action(StringView id) const
{
    return find(id) != nullptr;
}

Vector<String> ExtensionActionRegistry::extensions_with_actions() const
{
    Vector<String> ids;
    for (auto const& entry : m_entries)
        ids.append(entry.id);
    return ids;
}

ExtensionActionRegistry::Entry const* ExtensionActionRegistry::find(StringView id) const
{
    auto it = m_entries.find_if([&](auto const& entry) { return entry.id == id; });
    return it == m_entries.end() ? nullptr : &*it;
}

ExtensionActionRegistry::Entry* ExtensionActionRegistry::find(StringView id)
{
    auto it = m_entries.find_if([&](auto const& entry) { return entry.id == id; });
    return it == m_entries.end() ? nullptr : &*it;
}

void ExtensionActionRegistry::set(StringView id, Optional<i64> tab_id, ExtensionActionProperty property, Optional<JsonValue> value)
{
    auto* entry = find(id);
    if (!entry)
        return;
    auto& values = entry->values[to_underlying(property)];
    if (tab_id.has_value()) {
        if (value.has_value())
            values.per_tab.set(*tab_id, value.release_value());
        else
            values.per_tab.remove(*tab_id);
    } else {
        values.global = move(value);
    }
    if (on_change)
        on_change(entry->id, tab_id);
}

Optional<JsonValue> ExtensionActionRegistry::effective_value(Entry const& entry, Optional<i64> tab_id, ExtensionActionProperty property) const
{
    auto const& values = entry.values[to_underlying(property)];
    if (tab_id.has_value()) {
        if (auto value = values.per_tab.get(*tab_id); value.has_value())
            return *value;
    }
    return values.global;
}

Optional<JsonValue> ExtensionActionRegistry::get(StringView id, Optional<i64> tab_id, ExtensionActionProperty property) const
{
    auto const* entry = find(id);
    if (!entry)
        return {};
    if (auto value = effective_value(*entry, tab_id, property); value.has_value())
        return value;

    // Without a value of its own, a property reads as the state the button shows.
    auto state = this->state(id, tab_id);
    auto color_to_json = [](Gfx::Color color) {
        JsonArray array;
        for (auto component : { color.red(), color.green(), color.blue(), color.alpha() })
            array.must_append(component);
        return JsonValue { move(array) };
    };
    switch (property) {
    case ExtensionActionProperty::Title:
        return JsonValue { state.title };
    case ExtensionActionProperty::Popup:
        return JsonValue { state.popup };
    case ExtensionActionProperty::BadgeText:
        return JsonValue { state.badge_text };
    case ExtensionActionProperty::BadgeBackgroundColor:
        return color_to_json(state.badge_background_color);
    case ExtensionActionProperty::BadgeTextColor:
        return color_to_json(state.badge_text_color);
    case ExtensionActionProperty::Enabled:
        return JsonValue { state.enabled };
    case ExtensionActionProperty::Icon:
        return {};
    }
    VERIFY_NOT_REACHED();
}

ExtensionActionState ExtensionActionRegistry::state(StringView id, Optional<i64> tab_id) const
{
    ExtensionActionState state;
    auto const* entry = find(id);
    if (!entry)
        return state;
    auto string_or = [&](ExtensionActionProperty property, String fallback) {
        auto value = effective_value(*entry, tab_id, property);
        return value.has_value() && value->is_string() ? value->as_string() : fallback;
    };

    state.title = string_or(ExtensionActionProperty::Title, entry->action.default_title.value_or(entry->name));
    state.popup = string_or(ExtensionActionProperty::Popup, entry->action.default_popup.value_or({}));
    state.badge_text = string_or(ExtensionActionProperty::BadgeText, {});

    if (auto icon = effective_value(*entry, tab_id, ExtensionActionProperty::Icon); icon.has_value())
        state.icon = icon_from_json(*icon);
    else
        state.icon = entry->action.default_icon;

    // A badge is always drawn opaque, whatever alpha its color was given.
    state.badge_background_color = color_from_json(effective_value(*entry, tab_id, ExtensionActionProperty::BadgeBackgroundColor))
                                       .value_or(default_badge_background_color)
                                       .with_alpha(255);
    state.badge_text_color = color_from_json(effective_value(*entry, tab_id, ExtensionActionProperty::BadgeTextColor))
                                 .value_or(default_badge_text_color(state.badge_background_color));

    if (auto enabled = effective_value(*entry, tab_id, ExtensionActionProperty::Enabled); enabled.has_value() && enabled->is_bool())
        state.enabled = enabled->as_bool();
    return state;
}

void ExtensionActionRegistry::clear_tab(i64 tab_id)
{
    for (auto& entry : m_entries) {
        bool changed = false;
        for (auto& values : entry.values)
            changed |= values.per_tab.remove(tab_id);
        if (changed && on_change)
            on_change(entry.id, tab_id);
    }
}

}
