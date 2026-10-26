/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/Utf8View.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Extensions/ExtensionTabs.h>

namespace WebView {

static i64 s_next_extension_tab_id { 1 };
static i64 s_next_extension_window_id { 1 };

i64 allocate_extension_tab_id()
{
    return s_next_extension_tab_id++;
}

i64 allocate_extension_window_id()
{
    return s_next_extension_window_id++;
}

i64 extension_frame_id_for(Web::HTML::CrossProcessId navigable_id)
{
    static NeverDestroyed<HashMap<Web::HTML::CrossProcessId, i64>> frame_ids;
    static i64 next_frame_id { 1 };
    return frame_ids->ensure(navigable_id, [] { return next_frame_id++; });
}

bool may_see_tab(ExtensionTabViewer const& viewer, ExtensionTab const& tab)
{
    return tab.is_private == IsPrivate::No || viewer.allowed_in_private;
}

bool may_see_tab_details(ExtensionTabViewer const& viewer, ExtensionTab const& tab)
{
    if (viewer.extension.has_permission("tabs"sv))
        return true;
    // An extension always sees its own pages.
    if (tab.url.scheme() == Web::Extensions::scheme && tab.url.serialized_host() == viewer.extension.id)
        return true;
    return viewer.extension.host_permissions.contains([&](auto const& pattern) { return pattern.matches(tab.url); });
}

Optional<i64> last_focused_extension_window_for(ReadonlySpan<i64> focus_order, ReadonlySpan<ExtensionTab> tabs, bool allowed_in_private)
{
    for (auto window_id : focus_order) {
        auto tab = tabs.first_matching([&](auto const& tab) { return tab.window_id == window_id; });
        if (tab.has_value() && (tab->is_private == IsPrivate::No || allowed_in_private))
            return window_id;
    }
    return {};
}

static StringView status_of(ExtensionTab const& tab)
{
    if (!tab.view_id.has_value())
        return "unloaded"sv;
    return tab.loading ? "loading"sv : "complete"sv;
}

JsonObject serialize_extension_tab(ExtensionTabViewer const& viewer, ExtensionTab const& tab)
{
    JsonObject object;
    object.set("id"sv, tab.id);
    object.set("index"sv, tab.index);
    object.set("windowId"sv, tab.window_id);
    object.set("status"sv, status_of(tab));
    object.set("active"sv, tab.active);
    // With no multiple selection, the selected and highlighted tab of a window is its active one.
    object.set("selected"sv, tab.active);
    object.set("highlighted"sv, tab.active);
    object.set("pinned"sv, tab.pinned);
    object.set("discarded"sv, !tab.view_id.has_value());
    object.set("groupId"sv, -1);
    object.set("incognito"sv, tab.is_private == IsPrivate::Yes);
    if (may_see_tab_details(viewer, tab)) {
        object.set("url"sv, tab.url.serialize());
        object.set("title"sv, tab.title);
    }
    return object;
}

// The query comes from a renderer, which checked it, but whose checks the UI process can't rely on.
static ErrorOr<Vector<Web::Extensions::MatchPattern>, String> url_patterns_from(JsonValue const& value)
{
    Vector<String> spellings;
    if (value.is_string()) {
        spellings.append(value.as_string());
    } else if (value.is_array()) {
        for (auto const& item : value.as_array().values()) {
            if (!item.is_string())
                return "url must be a string or a list of strings"_string;
            spellings.append(item.as_string());
        }
    } else {
        return "url must be a string or a list of strings"_string;
    }
    Vector<Web::Extensions::MatchPattern> patterns;
    for (auto const& spelling : spellings) {
        auto pattern = Web::Extensions::MatchPattern::parse(spelling, Web::Extensions::MatchPattern::AllowExtensionScheme::Yes);
        // Scripts see this message and compare it, so its wording is fixed.
        if (pattern.is_error())
            return MUST(String::formatted("Invalid url pattern '{}'", spelling));
        patterns.append(pattern.release_value());
    }
    return patterns;
}

// Matches the way tab title filters do: "*" stands for any run of characters, and "?" for one character or none.
static bool title_matches(ReadonlySpan<u32> pattern, ReadonlySpan<u32> title)
{
    if (pattern.is_empty())
        return title.is_empty();
    auto first = pattern[0];
    auto rest = pattern.slice(1);
    if (first == '*') {
        for (size_t start = 0; start <= title.size(); ++start) {
            if (title_matches(rest, title.slice(start)))
                return true;
        }
        return false;
    }
    if (first == '?')
        return title_matches(rest, title) || (!title.is_empty() && title_matches(rest, title.slice(1)));
    return !title.is_empty() && title[0] == first && title_matches(rest, title.slice(1));
}

static bool title_matches(StringView pattern, StringView title)
{
    auto code_points_of = [](StringView string) {
        Vector<u32> code_points;
        for (auto code_point : Utf8View { string })
            code_points.append(code_point);
        return code_points;
    };
    auto pattern_code_points = code_points_of(pattern);
    auto title_code_points = code_points_of(title);
    return title_matches(pattern_code_points.span(), title_code_points.span());
}

ErrorOr<JsonArray, String> query_extension_tabs(ExtensionTabViewer const& viewer, ReadonlySpan<ExtensionTab> tabs, JsonObject const& query)
{
    // An empty url list or title, an index of -1, and a negative window id other than -2 restrict nothing.
    Optional<Vector<Web::Extensions::MatchPattern>> url_patterns;
    if (auto url = query.get("url"sv); url.has_value()) {
        url_patterns = TRY(url_patterns_from(*url));
        if (url_patterns->is_empty())
            url_patterns.clear();
    }
    auto title = query.get_string("title"sv);
    if (title.has_value() && title->is_empty())
        title.clear();

    auto current_window_id = viewer.current_window_id.has_value() ? viewer.current_window_id : viewer.last_focused_window_id;
    auto in_window = [](Optional<i64> window_id, ExtensionTab const& tab) { return window_id.has_value() && tab.window_id == *window_id; };

    auto matches = [&](ExtensionTab const& tab) {
        if (!may_see_tab(viewer, tab))
            return false;
        if (auto active = query.get_bool("active"sv); active.has_value() && tab.active != *active)
            return false;
        if (auto highlighted = query.get_bool("highlighted"sv); highlighted.has_value() && tab.active != *highlighted)
            return false;
        if (auto pinned = query.get_bool("pinned"sv); pinned.has_value() && tab.pinned != *pinned)
            return false;
        if (auto discarded = query.get_bool("discarded"sv); discarded.has_value() && tab.view_id.has_value() == *discarded)
            return false;
        if (auto current = query.get_bool("currentWindow"sv); current.has_value() && in_window(current_window_id, tab) != *current)
            return false;
        if (auto last_focused = query.get_bool("lastFocusedWindow"sv); last_focused.has_value() && in_window(viewer.last_focused_window_id, tab) != *last_focused)
            return false;
        if (auto window_id = query.get_integer<i64>("windowId"sv); window_id.has_value()) {
            // -2 is WINDOW_ID_CURRENT.
            if (*window_id == -2 ? !in_window(current_window_id, tab) : (*window_id >= 0 && tab.window_id != *window_id))
                return false;
        }
        if (auto index = query.get_integer<i64>("index"sv); index.has_value() && *index > -1 && tab.index != *index)
            return false;
        if (auto status = query.get_string("status"sv); status.has_value() && status_of(tab) != *status)
            return false;
        if (auto group_id = query.get_integer<i64>("groupId"sv); group_id.has_value() && *group_id != -1)
            return false;
        if (auto window_type = query.get_string("windowType"sv); window_type.has_value() && *window_type != (tab.in_popup_window ? "popup"sv : "normal"sv))
            return false;
        // A filter on what the caller can't see never matches.
        if (url_patterns.has_value() && (!may_see_tab_details(viewer, tab) || !url_patterns->contains([&](auto const& pattern) { return pattern.matches(tab.url); })))
            return false;
        if (title.has_value() && (!may_see_tab_details(viewer, tab) || !title_matches(*title, tab.title)))
            return false;
        return true;
    };

    JsonArray results;
    for (auto const& tab : tabs) {
        if (matches(tab))
            results.must_append(serialize_extension_tab(viewer, tab));
    }
    return results;
}

}
