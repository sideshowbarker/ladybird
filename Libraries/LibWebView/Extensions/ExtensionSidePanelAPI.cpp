/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionSidePanelAPI.h>
#include <LibWebView/Extensions/ExtensionSidePanelHost.h>

namespace WebView {

static Optional<i64> integer_of(JsonObject const& object, StringView key)
{
    return object.get_integer<i64>(key);
}

// An icon's paths as the API passes them: one path, or a map from sizes to paths.
static Vector<ExtensionManifest::IconPath> icon_paths_from(JsonValue const& value)
{
    Vector<ExtensionManifest::IconPath> paths;
    if (value.is_string()) {
        paths.append({ 0, MUST(String::from_utf8(value.as_string().bytes_as_string_view().trim("/"sv, TrimMode::Left))) });
    } else if (value.is_object()) {
        value.as_object().for_each_member([&](String const& size, JsonValue const& path) {
            if (auto pixels = size.to_number<u32>(); pixels.has_value() && path.is_string())
                paths.append({ *pixels, MUST(String::from_utf8(path.as_string().bytes_as_string_view().trim("/"sv, TrimMode::Left))) });
        });
        quick_sort(paths, [](auto const& a, auto const& b) { return a.size < b.size; });
    }
    return paths;
}

static ErrorOr<JsonValue, String> call_side_panel(ExtensionSidePanelHost& host, StringView id, StringView method, JsonObject const& options)
{
    if (method == "setOptions"sv) {
        host.set_options(id, integer_of(options, "tabId"sv), options.get_string("path"sv).map([](auto const& path) { return String(path); }), options.get_bool("enabled"sv), ExtensionSidePanelHost::OverrideSource::SidePanel);
        return JsonValue {};
    }
    if (method == "getOptions"sv) {
        auto tab_id = integer_of(options, "tabId"sv);
        auto resolved = host.options(id, tab_id);
        JsonObject result;
        if (resolved.path.has_value())
            result.set("path"sv, *resolved.path);
        result.set("enabled"sv, resolved.enabled);
        if (tab_id.has_value() && host.has_override(id, *tab_id))
            result.set("tabId"sv, *tab_id);
        return JsonValue { move(result) };
    }
    if (method == "setPanelBehavior"sv) {
        if (auto opens = options.get_bool("openPanelOnActionClick"sv); opens.has_value())
            host.set_opens_on_action_click(id, *opens);
        return JsonValue {};
    }
    if (method == "getPanelBehavior"sv) {
        JsonObject result;
        result.set("openPanelOnActionClick"sv, host.opens_on_action_click(id));
        return JsonValue { move(result) };
    }
    if (method == "open"sv) {
        TRY(host.open(id, integer_of(options, "windowId"sv), integer_of(options, "tabId"sv)));
        return JsonValue {};
    }
    if (method == "close"sv) {
        TRY(host.close(id, integer_of(options, "windowId"sv), integer_of(options, "tabId"sv)));
        return JsonValue {};
    }
    if (method == "getLayout"sv) {
        JsonObject result;
        result.set("side"sv, "right"sv);
        return JsonValue { move(result) };
    }
    return MUST(String::formatted("sidePanel.{}() isn't supported", method));
}

static ErrorOr<JsonValue, String> call_sidebar_action(ExtensionSidePanelHost& host, StringView id, StringView method, JsonObject const& details, ExtensionSidePanelCaller const& caller)
{
    auto tab_id = integer_of(details, "tabId"sv);
    auto window_id = integer_of(details, "windowId"sv);
    // The window a call without ids means: the caller's, else the one the user focused last.
    auto current_window = caller.current_window_id.has_value() ? caller.current_window_id : caller.last_focused_window_id;

    if (method == "setPanel"sv) {
        auto panel = details.get("panel"sv);
        // A null panel restores the manifest's; per window, the panel is the global one.
        if (panel.has_value() && panel->is_null()) {
            if (tab_id.has_value())
                host.clear_override(id, *tab_id);
            else
                host.reset_options(id);
            return JsonValue {};
        }
        if (!panel.has_value() || !panel->is_string())
            return "Invalid arguments"_string;
        host.set_options(id, tab_id, Optional<String> { panel->as_string() }, {}, ExtensionSidePanelHost::OverrideSource::SidebarAction);
        return JsonValue {};
    }
    if (method == "getPanel"sv) {
        if (auto url = host.panel_url(id, tab_id); url.has_value())
            return JsonValue { url.release_value() };
        return JsonValue { ""_string };
    }
    if (method == "setTitle"sv) {
        auto title = details.get("title"sv);
        host.set_title(id, tab_id, title.has_value() && title->is_string() ? Optional<String> { title->as_string() } : Optional<String> {});
        return JsonValue {};
    }
    if (method == "getTitle"sv)
        return JsonValue { host.title(id, tab_id) };
    if (method == "setIcon"sv) {
        if (auto path = details.get("path"sv); path.has_value() && !path->is_null())
            host.set_icon(id, tab_id, icon_paths_from(*path));
        else
            host.set_icon(id, tab_id, {});
        return JsonValue {};
    }
    if (method == "isOpen"sv) {
        auto window = window_id.has_value() ? window_id : current_window;
        return JsonValue { window.has_value() && host.is_open(id, *window) };
    }
    if (method == "open"sv || method == "close"sv || method == "toggle"sv) {
        if (!current_window.has_value())
            return "No current window"_string;
        if (method == "open"sv)
            TRY(host.open(id, current_window, {}));
        else if (method == "close"sv)
            TRY(host.close(id, current_window, {}));
        else
            host.toggle(id, *current_window);
        return JsonValue {};
    }
    return MUST(String::formatted("sidebarAction.{}() isn't supported", method));
}

ErrorOr<JsonValue, String> call_extension_side_panel_api(ExtensionSidePanelHost& host, Extension const& extension, StringView api_namespace, StringView method, JsonArray const& arguments, ExtensionSidePanelCaller const& caller)
{
    JsonObject options;
    if (arguments.size() > 0) {
        if (!arguments.at(0).is_object())
            return "Invalid arguments"_string;
        options = arguments.at(0).as_object();
    }
    if (api_namespace == "sidePanel"sv)
        return call_side_panel(host, extension.id(), method, options);
    return call_sidebar_action(host, extension.id(), method, options, caller);
}

}
