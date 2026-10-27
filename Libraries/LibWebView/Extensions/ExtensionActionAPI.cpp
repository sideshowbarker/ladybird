/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Extensions/ExtensionActionAPI.h>
#include <LibWebView/Extensions/ExtensionActionRegistry.h>

namespace WebView {

bool may_call_extension_action_api(bool is_extension_document, StringView api_namespace, Web::Extensions::ExtensionDescriptor const& descriptor)
{
    if (!is_extension_document || !descriptor.has_action)
        return false;
    return api_namespace == (descriptor.manifest_version == 2 ? "browserAction"sv : "action"sv);
}

static String extension_origin(Web::Extensions::ExtensionDescriptor const& descriptor)
{
    return MUST(String::formatted("{}://{}/", Web::Extensions::scheme, descriptor.id));
}

// An extension names its pages and icons by path or by its own URL; the registry keeps paths from its root.
static String extension_relative_path(Web::Extensions::ExtensionDescriptor const& descriptor, StringView path)
{
    auto origin = extension_origin(descriptor);
    if (path.starts_with(origin))
        path = path.substring_view(origin.bytes_as_string_view().length());
    return MUST(String::from_utf8(path.trim("/"sv, TrimMode::Left)));
}

static bool is_color(JsonValue const& value)
{
    if (!value.is_array() || value.as_array().size() != 4)
        return false;
    for (auto const& component : value.as_array().values()) {
        if (!component.get_integer<u8>().has_value())
            return false;
    }
    return true;
}

ErrorOr<JsonValue, String> call_extension_action_api(ExtensionActionRegistry& registry, Web::Extensions::ExtensionDescriptor const& descriptor, StringView method, JsonArray const& arguments)
{
    if (arguments.is_empty())
        return "Invalid arguments"_string;
    Optional<i64> tab_id;
    if (!arguments.at(0).is_null()) {
        auto id = arguments.at(0).get_integer<i64>();
        if (!id.has_value())
            return "Invalid tab id"_string;
        tab_id = *id;
    }
    Optional<JsonValue> value;
    if (arguments.size() > 1 && !arguments.at(1).is_null())
        value = arguments.at(1);

    using Property = ExtensionActionProperty;
    struct Accessor {
        StringView setter;
        StringView getter;
        Property property;
    };
    static constexpr Array accessors {
        Accessor { "setTitle"sv, "getTitle"sv, Property::Title },
        Accessor { "setPopup"sv, "getPopup"sv, Property::Popup },
        Accessor { "setBadgeText"sv, "getBadgeText"sv, Property::BadgeText },
        Accessor { "setBadgeBackgroundColor"sv, "getBadgeBackgroundColor"sv, Property::BadgeBackgroundColor },
        Accessor { "setBadgeTextColor"sv, "getBadgeTextColor"sv, Property::BadgeTextColor },
        Accessor { "setIcon"sv, {}, Property::Icon },
    };

    // Manifest version 2's browserAction has no badge text color.
    if (descriptor.manifest_version == 2 && method.contains("BadgeTextColor"sv))
        return MUST(String::formatted("browserAction.{} isn't supported", method));

    for (auto const& accessor : accessors) {
        if (method == accessor.setter) {
            if (value.has_value()) {
                if ((accessor.property == Property::BadgeBackgroundColor || accessor.property == Property::BadgeTextColor) && !is_color(*value))
                    return "Invalid color"_string;
                if (accessor.property != Property::BadgeBackgroundColor && accessor.property != Property::BadgeTextColor && accessor.property != Property::Icon && !value->is_string())
                    return "Invalid value"_string;
                if (accessor.property == Property::Popup)
                    value = JsonValue { extension_relative_path(descriptor, value->as_string()) };
                if (accessor.property == Property::Icon) {
                    if (value->is_string()) {
                        value = JsonValue { extension_relative_path(descriptor, value->as_string()) };
                    } else if (value->is_object()) {
                        JsonObject sizes;
                        value->as_object().for_each_member([&](String const& size, JsonValue const& path) {
                            if (path.is_string())
                                sizes.set(size, extension_relative_path(descriptor, path.as_string()));
                        });
                        value = JsonValue { move(sizes) };
                    } else {
                        return "Invalid icon"_string;
                    }
                }
            }
            registry.set(descriptor.id, tab_id, accessor.property, move(value));
            return JsonValue {};
        }
        if (!accessor.getter.is_empty() && method == accessor.getter) {
            auto result = registry.get(descriptor.id, tab_id, accessor.property).value_or({});
            // A popup reads back as the page's full URL, or as the empty string when there's none.
            if (accessor.property == Property::Popup && result.is_string() && !result.as_string().is_empty())
                return JsonValue { MUST(String::formatted("{}{}", extension_origin(descriptor), result.as_string())) };
            return result;
        }
    }

    if (method == "enable"sv || method == "disable"sv) {
        registry.set(descriptor.id, tab_id, Property::Enabled, JsonValue { method == "enable"sv });
        return JsonValue {};
    }
    if (method == "isEnabled"sv)
        return JsonValue { registry.state(descriptor.id, tab_id).enabled };

    return MUST(String::formatted("Unknown action method: {}", method));
}

}
