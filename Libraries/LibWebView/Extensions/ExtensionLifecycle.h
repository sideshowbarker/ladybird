/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWebView/Export.h>

namespace WebView {

// What runtime.onInstalled reports for an extension loaded at a version, given the version it was last seen at: an
// install the first time, an update when the version changed, and nothing when it didn't.
WEBVIEW_API Optional<JsonObject> extension_installed_details(Optional<String> const& last_seen_version, String const& version);

// runtime.onStartup reaches only an extension the browser had seen before it started; a new one hears onInstalled.
WEBVIEW_API bool extension_hears_browser_start(Optional<String> const& last_seen_version);

}
