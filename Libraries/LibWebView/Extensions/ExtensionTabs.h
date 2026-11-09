/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibURL/URL.h>
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

}
