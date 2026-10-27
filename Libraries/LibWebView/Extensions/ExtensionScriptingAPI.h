/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/Injection.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Which frames of a tab an injection is for: the given frames, every frame, or else the top one. An about:blank or
// about:srcdoc frame counts as its creator's URL when match_about_blank is set, and is passed over otherwise.
struct ExtensionInjectionTarget {
    i64 tab_id { 0 };
    Optional<Vector<i64>> frame_ids;
    bool all_frames { false };
    bool match_about_blank { true };
};

// A scripting.executeScript() call, its files read from the extension.
struct ExtensionInjectionRequest {
    ExtensionInjectionTarget target;
    Web::Extensions::ExtensionInjection injection;
};

// Checks and reads the details object the renderer sent; extensions compare these errors' wording.
WEBVIEW_API ErrorOr<ExtensionInjectionRequest, String> parse_extension_injection(Extension const&, JsonObject const& details);

// Why an extension may not inject into a document at the URL, as the error extensions compare: another extension's
// page, or a host it has no access to (with the URL only when it may see the tab's URL).
// A browser page (about:, other than about:blank and about:srcdoc) is never reachable, activeTab or not.
WEBVIEW_API Optional<String> extension_injection_refusal(Extension const&, URL::URL const&, bool may_see_url, bool has_active_tab);

}
