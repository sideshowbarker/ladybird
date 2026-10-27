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
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Export.h>

namespace WebView {

// What tabs.create() asks for. A tab without a URL shows the new tab page.
struct ExtensionTabCreation {
    Optional<URL::URL> url;
    bool active { true };
    Optional<i64> index;
    Optional<i64> window_id;
    bool pinned { false };
    // A frontend that picks the window itself picks a private one only if the extension is allowed there.
    bool may_open_in_private_window { false };
};

// What tabs.update() changes; whatever it leaves empty stays as it is.
struct ExtensionTabUpdate {
    Optional<URL::URL> url;
    Optional<bool> active;
    Optional<bool> pinned;
};

// Resolves a URL that tabs.create() or tabs.update() got against the extension's own base URL, and rejects what an
// extension may not open in a tab: javascript: and file: URLs, and another extension's pages. Extensions compare these
// errors' wording.
WEBVIEW_API ErrorOr<URL::URL, String> resolve_extension_tab_url(Web::Extensions::ExtensionDescriptor const&, StringView url);

// The renderer has already checked the properties' names and value types.
WEBVIEW_API ErrorOr<ExtensionTabCreation, String> parse_extension_tab_creation(Web::Extensions::ExtensionDescriptor const&, JsonObject const& properties);
WEBVIEW_API ErrorOr<ExtensionTabUpdate, String> parse_extension_tab_update(Web::Extensions::ExtensionDescriptor const&, JsonObject const& properties);

}
