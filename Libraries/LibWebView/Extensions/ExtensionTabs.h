/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>

namespace WebView {

// A browser tab as extensions see it. A tab keeps its id across navigations, process swaps, and moves between windows,
// and no tab or window id is ever reused while the browser runs.
struct ExtensionTab {
    i64 id { 0 };
    i64 window_id { 0 };
    i64 index { 0 };
    bool active { false };
    bool pinned { false };
    IsPrivate is_private { IsPrivate::No };
    // A tab restored from a session has no view until it's first shown.
    Optional<u64> view_id;
    URL::URL url;
    String title;
    bool loading { false };
    bool in_popup_window { false };
};

WEBVIEW_API i64 allocate_extension_tab_id();
WEBVIEW_API i64 allocate_extension_window_id();

// The frame id extensions see for a navigable that isn't its tab's top-level one: positive, kept for as long as the
// browser runs, and never reused.
WEBVIEW_API i64 extension_frame_id_for(Web::HTML::CrossProcessId navigable_id);

// The extension asking about tabs, and where it's asking from: the window of the tab its page is in, if it's in one, and
// the window the user last used.
// The tabs whose URL and title an extension's activeTab permission shows it: each tab where the user invoked the
// extension, until the tab navigates to another origin or closes.
class WEBVIEW_API ExtensionActiveTabGrants {
public:
    void grant(i64 tab_id, URL::URL const&);
    void tab_navigated(i64 tab_id, URL::URL const&);
    void tab_closed(i64 tab_id);
    bool contains(i64 tab_id) const { return m_origins.contains(tab_id); }

private:
    HashMap<i64, URL::Origin> m_origins;
};

struct ExtensionTabViewer {
    Web::Extensions::ExtensionDescriptor const& extension;
    bool allowed_in_private { false };
    Optional<i64> current_window_id;
    Optional<i64> last_focused_window_id;
    ExtensionActiveTabGrants const* active_tab_grants { nullptr };
};

// A tab of a private window exists for an extension only if it's allowed there, and its URL and title only with the tabs
// permission or a host permission that matches the URL.
WEBVIEW_API bool may_see_tab(ExtensionTabViewer const&, ExtensionTab const&);
WEBVIEW_API bool may_see_tab_details(ExtensionTabViewer const&, ExtensionTab const&);

// The window the user last focused among those the extension can see: A private window it isn't allowed in doesn't
// count, so focusing one neither hides the other windows nor shows the extension that it was focused.
WEBVIEW_API Optional<i64> last_focused_extension_window_for(ReadonlySpan<i64> focus_order, ReadonlySpan<ExtensionTab>, bool allowed_in_private);

WEBVIEW_API JsonObject serialize_extension_tab(ExtensionTabViewer const&, ExtensionTab const&);

// A browser window as extensions see it, made from the tabs it holds: A window exists for an extension when one of its
// tabs does, and the focused one is the one the user last focused.
struct ExtensionWindow {
    i64 id { 0 };
    bool focused { false };
    IsPrivate is_private { IsPrivate::No };
    bool is_popup { false };
};
WEBVIEW_API Vector<ExtensionWindow> extension_windows(ExtensionTabViewer const&, ReadonlySpan<ExtensionTab>);
// With tabs, the window lists the extension's view of each of its tabs, as populate asks for.
WEBVIEW_API JsonObject serialize_extension_window(ExtensionTabViewer const&, ExtensionWindow const&, Optional<ReadonlySpan<ExtensionTab>> tabs = {});

// What tabs.onUpdated reports to the extension of a tab's change: the status, URL, title, and pin that changed, without
// the URL and title when it may not see them. It's empty when the extension may not see the tab, or nothing it sees
// changed.
WEBVIEW_API Optional<JsonObject> extension_tab_change_info(ExtensionTabViewer const&, ExtensionTab const& before, ExtensionTab const& after);

// The tabs that match a tabs.query() argument, whose keys and value types the renderer has already checked.
WEBVIEW_API ErrorOr<JsonArray, String> query_extension_tabs(ExtensionTabViewer const&, ReadonlySpan<ExtensionTab>, JsonObject const& query);

}
