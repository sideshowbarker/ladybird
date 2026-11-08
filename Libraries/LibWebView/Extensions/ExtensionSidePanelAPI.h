/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Where a call comes from: the window the calling page is in, if any, and the window the user focused last.
struct ExtensionSidePanelCaller {
    Optional<i64> current_window_id;
    Optional<i64> last_focused_window_id;
};

// Runs one call to the sidePanel or sidebarAction API ("open", e.g., without its namespace), whose arguments are the
// options or details object the page gave. Returns what the call resolves with, or the message it rejects with.
WEBVIEW_API ErrorOr<JsonValue, String> call_extension_side_panel_api(ExtensionSidePanelHost&, Extension const&, StringView api_namespace, StringView method, JsonArray const& arguments, ExtensionSidePanelCaller const&);

}
