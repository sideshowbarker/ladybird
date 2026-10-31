/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionManagementAPI.h>

namespace WebView {

JsonObject extension_info_for(Extension const& extension, bool installed_from_package)
{
    auto const& descriptor = extension.descriptor();
    auto manifest = JsonValue::from_string(descriptor.manifest_json);
    auto const* object = manifest.is_error() || !manifest.value().is_object() ? nullptr : &manifest.value().as_object();
    auto manifest_string = [&](StringView key) { return object ? object->get_string(key).value_or({}) : String {}; };

    JsonObject info;
    info.set("id"sv, extension.id());
    info.set("name"sv, descriptor.name);
    info.set("shortName"sv, manifest_string("short_name"sv));
    info.set("description"sv, extension.manifest().description);
    info.set("version"sv, descriptor.version);
    info.set("mayDisable"sv, true);
    info.set("enabled"sv, true);
    auto const& options_page = extension.manifest().options_page;
    info.set("optionsUrl"sv, options_page.has_value() ? MUST(String::formatted("{}://{}/{}", Web::Extensions::scheme, extension.id(), options_page->bytes_as_string_view().trim("/"sv, TrimMode::Left))) : String {});
    info.set("installType"sv, installed_from_package ? "normal"sv : "development"sv);
    info.set("type"sv, "extension"sv);

    JsonArray permissions;
    for (auto const& permission : descriptor.api_permissions)
        permissions.must_append(permission);
    info.set("permissions"sv, move(permissions));
    JsonArray host_permissions;
    for (auto const* patterns : { &descriptor.host_permissions, &descriptor.granted_origins }) {
        for (auto const& pattern : *patterns)
            host_permissions.must_append(pattern.to_string());
    }
    info.set("hostPermissions"sv, move(host_permissions));

    if (auto icons = object ? object->get_object("icons"sv) : Optional<JsonObject const&> {}; icons.has_value()) {
        JsonArray list;
        icons->for_each_member([&](String const& size, JsonValue const& path) {
            if (!path.is_string())
                return;
            JsonObject icon;
            icon.set("size"sv, size.to_number<u32>().value_or(0));
            icon.set("url"sv, path.as_string());
            list.must_append(move(icon));
        });
        info.set("icons"sv, move(list));
    }
    if (auto homepage = manifest_string("homepage_url"sv); !homepage.is_empty())
        info.set("homepageUrl"sv, homepage);
    // The update URL the manifest's Gecko settings name, as either of their keys holds them.
    for (auto key : { "browser_specific_settings"sv, "applications"sv }) {
        auto settings = object ? object->get_object(key) : Optional<JsonObject const&> {};
        auto gecko = settings.has_value() ? settings->get_object("gecko"sv) : Optional<JsonObject const&> {};
        if (auto update_url = gecko.has_value() ? gecko->get_string("update_url"sv) : Optional<String> {}; update_url.has_value()) {
            info.set("updateUrl"sv, *update_url);
            break;
        }
    }
    return info;
}

}
