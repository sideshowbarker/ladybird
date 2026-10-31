/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionPrivacySettings.h>

namespace WebView {

static constexpr auto network_prediction_enabled_name = "network.networkPredictionEnabled"sv;
static constexpr auto hyperlink_auditing_enabled_name = "websites.hyperlinkAuditingEnabled"sv;

static JsonValue default_value(StringView name)
{
    if (name == "network.webRTCIPHandlingPolicy"sv)
        return "default"_string;
    // Ladybird never sends hyperlink auditing pings, whatever an extension sets.
    if (name == hyperlink_auditing_enabled_name)
        return false;
    return true;
}

static constexpr Array setting_names {
    network_prediction_enabled_name,
    "network.peerConnectionEnabled"sv,
    "network.webRTCIPHandlingPolicy"sv,
    hyperlink_auditing_enabled_name,
};

ReadonlySpan<StringView> ExtensionPrivacySettings::names()
{
    return setting_names;
}

bool ExtensionPrivacySettings::is_known(StringView name)
{
    return names().contains_slow(name);
}

ExtensionPrivacySettings::ExtensionPrivacySettings(InstallOrder install_order)
    : m_install_order(move(install_order))
{
}

Optional<size_t> ExtensionPrivacySettings::install_index(StringView extension_id) const
{
    auto order = m_install_order();
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == extension_id)
            return i;
    }
    return {};
}

ExtensionPrivacySettings::Effective ExtensionPrivacySettings::effective(StringView name) const
{
    if (name == hyperlink_auditing_enabled_name)
        return { {}, default_value(name) };
    auto values = m_values.get(name);
    if (!values.has_value())
        return { {}, default_value(name) };
    Optional<String> controller;
    Optional<size_t> controller_index;
    for (auto const& [extension_id, value] : *values) {
        auto index = install_index(extension_id);
        if (index.has_value() && (!controller_index.has_value() || *index > *controller_index)) {
            controller = extension_id;
            controller_index = index;
        }
    }
    if (!controller.has_value())
        return { {}, default_value(name) };
    return { controller, values->get(*controller).value() };
}

JsonObject ExtensionPrivacySettings::get(StringView extension_id, StringView name) const
{
    auto current = effective(name);
    StringView level_of_control;
    if (name == hyperlink_auditing_enabled_name) {
        level_of_control = "not_controllable"sv;
    } else if (!current.controller.has_value()) {
        level_of_control = "controllable_by_this_extension"sv;
    } else if (*current.controller == extension_id) {
        level_of_control = "controlled_by_this_extension"sv;
    } else {
        // An extension installed after the one in control may take it over.
        auto own_index = install_index(extension_id);
        auto controller_index = install_index(*current.controller);
        level_of_control = own_index.has_value() && controller_index.has_value() && *own_index > *controller_index
            ? "controllable_by_this_extension"sv
            : "controlled_by_other_extensions"sv;
    }
    JsonObject details;
    details.set("levelOfControl"sv, level_of_control);
    details.set("value"sv, current.value);
    return details;
}

void ExtensionPrivacySettings::announce_if_changed(StringView name)
{
    auto now = effective(name);
    auto key = MUST(String::from_utf8(name));
    auto announced = m_announced.get(key).value_or(Effective { {}, default_value(name) });
    if (now == announced)
        return;
    m_announced.set(key, move(now));
    if (on_effective_value_change)
        on_effective_value_change(name);
}

bool ExtensionPrivacySettings::set(String const& extension_id, StringView name, JsonValue value)
{
    if (name == hyperlink_auditing_enabled_name)
        return false;
    m_values.ensure(MUST(String::from_utf8(name))).set(extension_id, move(value));
    announce_if_changed(name);
    return effective(name).controller == extension_id;
}

bool ExtensionPrivacySettings::clear(StringView extension_id, StringView name)
{
    auto values = m_values.find(name);
    if (values == m_values.end())
        return false;
    auto removed = values->value.remove(extension_id);
    announce_if_changed(name);
    return removed;
}

void ExtensionPrivacySettings::remove_extension(StringView extension_id)
{
    for (auto name : names())
        clear(extension_id, name);
}

void ExtensionPrivacySettings::retain_extensions(Function<bool(StringView extension_id)> const& may_keep)
{
    for (auto const& extension_id : setters()) {
        if (!may_keep(extension_id))
            remove_extension(extension_id);
    }
}

Vector<String> ExtensionPrivacySettings::setters() const
{
    Vector<String> setters;
    for (auto const& [name, values] : m_values) {
        for (auto const& [extension_id, value] : values) {
            if (!setters.contains_slow(extension_id))
                setters.append(extension_id);
        }
    }
    return setters;
}

bool ExtensionPrivacySettings::network_prediction_enabled() const
{
    auto value = effective(network_prediction_enabled_name).value;
    return !value.is_bool() || value.as_bool();
}

}
