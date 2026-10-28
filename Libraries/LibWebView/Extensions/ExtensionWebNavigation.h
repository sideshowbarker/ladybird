/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <LibWebView/Forward.h>

namespace WebView {

// A frame as webNavigation names it: 0 for a tab's top-level document, whose parent is -1.
struct WebNavigationFrameIds {
    i64 frame_id { 0 };
    i64 parent_frame_id { -1 };
};

WEBVIEW_API WebNavigationFrameIds web_navigation_frame_ids(CanonicalNavigable const&);

// webNavigation.getFrame()'s and getAllFrames()'s results for the frames of a tab; a frame the tab doesn't have is null.
WEBVIEW_API JsonValue web_navigation_frame(CanonicalTraversable const&, i64 frame_id);
WEBVIEW_API JsonArray web_navigation_all_frames(CanonicalTraversable const&);

}
