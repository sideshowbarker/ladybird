/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionPermissionsAPI.h>

namespace WebView {

JsonObject ExtensionPermissionSet::to_json() const
{
    JsonArray permission_array;
    for (auto const& permission : permissions)
        permission_array.must_append(permission);
    JsonArray origin_array;
    for (auto const& origin : origins)
        origin_array.must_append(origin);
    JsonObject object;
    object.set("permissions"sv, move(permission_array));
    object.set("origins"sv, move(origin_array));
    return object;
}

ExtensionPermissionSet ExtensionPermissionSet::from_json(JsonObject const& object)
{
    ExtensionPermissionSet set;
    auto strings_of = [&](StringView key, Vector<String>& strings) {
        if (auto array = object.get_array(key); array.has_value()) {
            array->for_each([&](JsonValue const& value) {
                if (value.is_string())
                    strings.append(value.as_string());
            });
        }
    };
    strings_of("permissions"sv, set.permissions);
    strings_of("origins"sv, set.origins);
    return set;
}

ExtensionPermissionSet extension_active_permissions(Extension const& extension)
{
    auto const& descriptor = extension.descriptor();
    ExtensionPermissionSet set;
    set.permissions = descriptor.api_permissions;
    auto add_origin = [&](String origin) {
        if (!set.origins.contains_slow(origin))
            set.origins.append(move(origin));
    };
    for (auto const& pattern : descriptor.host_permissions)
        add_origin(pattern.to_string());
    for (auto const& pattern : descriptor.granted_origins)
        add_origin(pattern.to_string());
    // A manifest's content scripts imply host access; registered ones need it already.
    for (auto const& script : descriptor.content_scripts) {
        if (script.id.has_value())
            continue;
        for (auto const& pattern : script.matches)
            add_origin(pattern.to_string());
    }
    return set;
}

static bool reaches(Extension const& extension, Web::Extensions::MatchPattern const& requested)
{
    auto const& descriptor = extension.descriptor();
    auto covers = [&](auto const& pattern) { return pattern.contains(requested); };
    if (descriptor.host_permissions.contains(covers) || descriptor.granted_origins.contains(covers))
        return true;
    return any_of(descriptor.content_scripts, [&](auto const& script) { return !script.id.has_value() && script.matches.contains(covers); });
}

bool extension_has_permissions(Extension const& extension, ExtensionPermissionSet const& requested)
{
    for (auto const& permission : requested.permissions) {
        if (!extension.descriptor().has_permission(permission))
            return false;
    }
    for (auto const& origin : requested.origins) {
        auto pattern = Web::Extensions::MatchPattern::parse(origin);
        if (pattern.is_error() || !reaches(extension, pattern.value()))
            return false;
    }
    return true;
}

ErrorOr<ExtensionPermissionSet, String> extension_permissions_to_remove(Extension const& extension, ExtensionPermissionSet const& requested)
{
    auto const& manifest = extension.manifest();
    for (auto const& permission : requested.permissions) {
        if (!manifest.permissions.contains_slow(permission) && !manifest.optional_permissions.contains_slow(permission))
            return "Only permissions specified in the manifest may be requested."_string;
    }
    for (auto const& origin : requested.origins) {
        if (!manifest.host_permissions.contains_slow(origin) && !manifest.optional_host_permissions.contains_slow(origin))
            return "Only permissions specified in the manifest may be requested."_string;
    }
    for (auto const& permission : requested.permissions) {
        if (manifest.permissions.contains_slow(permission))
            return "You cannot remove required permissions."_string;
    }
    // An origin is required when a required host pattern, or a manifest content script's match, covers it or is
    // covered by it.
    auto const& descriptor = extension.descriptor();
    for (auto const& origin : requested.origins) {
        if (manifest.host_permissions.contains_slow(origin))
            return "You cannot remove required permissions."_string;
        auto pattern = Web::Extensions::MatchPattern::parse(origin);
        if (pattern.is_error())
            continue;
        auto overlaps = [&](Web::Extensions::MatchPattern const& required) { return required.contains(pattern.value()) || pattern.value().contains(required); };
        if (descriptor.host_permissions.contains(overlaps))
            return "You cannot remove required permissions."_string;
        for (auto const& script : descriptor.content_scripts) {
            if (!script.id.has_value() && script.matches.contains(overlaps))
                return "You cannot remove required permissions."_string;
        }
    }
    ExtensionPermissionSet removed;
    for (auto const& permission : requested.permissions) {
        if (extension.descriptor().has_permission(permission))
            removed.permissions.append(permission);
    }
    for (auto const& origin : requested.origins) {
        if (any_of(extension.descriptor().granted_origins, [&](auto const& pattern) { return pattern.to_string() == origin; }))
            removed.origins.append(origin);
    }
    return removed;
}

ExtensionPermissionSet extension_permissions_to_grant(Extension const& extension, ExtensionPermissionSet const& requested)
{
    auto const& manifest = extension.manifest();
    ExtensionPermissionSet granted;
    for (auto const& permission : requested.permissions) {
        if (manifest.optional_permissions.contains_slow(permission) && !extension.descriptor().has_permission(permission))
            granted.permissions.append(permission);
    }
    for (auto const& origin : requested.origins) {
        if (manifest.optional_host_permissions.contains_slow(origin) && !any_of(extension.descriptor().granted_origins, [&](auto const& pattern) { return pattern.to_string() == origin; }))
            granted.origins.append(origin);
    }
    return granted;
}

}
