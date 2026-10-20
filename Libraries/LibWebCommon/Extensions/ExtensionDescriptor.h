/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

// When a content script runs, relative to the document's load.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/content_scripts#run_at
enum class RunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

// The JavaScript world a content script runs in: its extension's isolated world, or the page's own main world.
enum class ExecutionWorld : u8 {
    Isolated,
    Main,
};

// One entry of a manifest's "content_scripts", with the script and style sheet sources already read from disk.
struct WEBCOMMON_API ContentScriptDescriptor {
    Vector<MatchPattern> matches;
    Vector<MatchPattern> exclude_matches;
    Vector<String> js_paths;
    Vector<String> js_sources;
    Vector<String> css_paths;
    Vector<String> css_sources;
    RunAt run_at { RunAt::DocumentIdle };
    bool all_frames { false };
    bool match_about_blank { false };
    ExecutionWorld world { ExecutionWorld::Isolated };

    bool operator==(ContentScriptDescriptor const&) const = default;
};

// One entry of a manifest's "web_accessible_resources": the resource path globs, and the page URL patterns
// that may load them.
struct WEBCOMMON_API WebAccessibleResourceDescriptor {
    Vector<String> resource_globs;
    Vector<MatchPattern> matches;

    bool operator==(WebAccessibleResourceDescriptor const&) const = default;
};

// Everything a WebContent process needs to know about an enabled extension.
struct WEBCOMMON_API ExtensionDescriptor {
    String id;
    u64 revision { 0 };
    String name;
    String version;
    u8 manifest_version { 3 };
    String manifest_json;
    String extension_pages_csp;
    Vector<WebAccessibleResourceDescriptor> web_accessible_resources;
    Vector<ContentScriptDescriptor> content_scripts;
    bool has_background { false };
    String background_document_html;

    bool operator==(ExtensionDescriptor const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::ContentScriptDescriptor const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::ContentScriptDescriptor> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebAccessibleResourceDescriptor const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebAccessibleResourceDescriptor> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::ExtensionDescriptor const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::ExtensionDescriptor> decode(Decoder&);

}
