/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Export.h>

namespace WebView {

// The parsed contents of an extension's manifest.json. Manifest versions 2 and 3 are both supported; the content
// scripts are described with their file paths only, and Extension reads the files.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json
struct WEBVIEW_API ExtensionManifest {
    struct Background {
        Vector<String> scripts;
        Optional<String> page;
        Optional<String> service_worker;
        bool is_module { false };
    };

    // Returns a user-facing error naming the offending key when the manifest is invalid.
    static ErrorOr<ExtensionManifest, String> parse(StringView manifest_json);

    // The HTML of the page that hosts "background.scripts" or "background.service_worker"; empty when the manifest
    // names its own "background.page" or has no background.
    String generated_background_document_html() const;

    u8 manifest_version { 3 };
    String name;
    String version;
    String description;
    String default_locale;
    Vector<Web::Extensions::ContentScriptDescriptor> content_scripts;
    Vector<Web::Extensions::WebAccessibleResourceDescriptor> web_accessible_resources;
    Vector<String> permissions;
    Vector<String> host_permissions;
    String extension_pages_csp;
    Optional<Background> background;
};

}
