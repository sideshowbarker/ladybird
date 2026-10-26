/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
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

// The extension asking about tabs, and where it's asking from: the window of the tab its page is in, if it's in one, and
// the window the user last used.
struct ExtensionTabViewer {
    Web::Extensions::ExtensionDescriptor const& extension;
    bool allowed_in_private { false };
    Optional<i64> current_window_id;
    Optional<i64> last_focused_window_id;
};

// A tab of a private window exists for an extension only if it's allowed there, and its URL and title only with the tabs
// permission or a host permission that matches the URL.
WEBVIEW_API bool may_see_tab(ExtensionTabViewer const&, ExtensionTab const&);
WEBVIEW_API bool may_see_tab_details(ExtensionTabViewer const&, ExtensionTab const&);

// The window the user last focused among those the extension can see: A private window it isn't allowed in doesn't
// count, so focusing one neither hides the other windows nor shows the extension that it was focused.
WEBVIEW_API Optional<i64> last_focused_extension_window_for(ReadonlySpan<i64> focus_order, ReadonlySpan<ExtensionTab>, bool allowed_in_private);

WEBVIEW_API JsonObject serialize_extension_tab(ExtensionTabViewer const&, ExtensionTab const&);

// The tabs that match a tabs.query() argument, whose keys and value types the renderer has already checked.
WEBVIEW_API ErrorOr<JsonArray, String> query_extension_tabs(ExtensionTabViewer const&, ReadonlySpan<ExtensionTab>, JsonObject const& query);

}
