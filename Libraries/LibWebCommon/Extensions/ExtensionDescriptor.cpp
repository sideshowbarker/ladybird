/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::ContentScriptDescriptor const& script)
{
    TRY(encoder.encode(script.matches));
    TRY(encoder.encode(script.exclude_matches));
    TRY(encoder.encode(script.js_paths));
    TRY(encoder.encode(script.js_sources));
    TRY(encoder.encode(script.css_paths));
    TRY(encoder.encode(script.css_sources));
    TRY(encoder.encode(script.run_at));
    TRY(encoder.encode(script.all_frames));
    TRY(encoder.encode(script.match_about_blank));
    TRY(encoder.encode(script.world));
    return {};
}

template<>
ErrorOr<Web::Extensions::ContentScriptDescriptor> decode(Decoder& decoder)
{
    Web::Extensions::ContentScriptDescriptor script;
    script.matches = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    script.exclude_matches = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    script.js_paths = TRY(decoder.decode<Vector<String>>());
    script.js_sources = TRY(decoder.decode<Vector<String>>());
    script.css_paths = TRY(decoder.decode<Vector<String>>());
    script.css_sources = TRY(decoder.decode<Vector<String>>());
    script.run_at = TRY(decoder.decode<Web::Extensions::RunAt>());
    script.all_frames = TRY(decoder.decode<bool>());
    script.match_about_blank = TRY(decoder.decode<bool>());
    script.world = TRY(decoder.decode<Web::Extensions::ExecutionWorld>());
    return script;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::WebAccessibleResourceDescriptor const& resource)
{
    TRY(encoder.encode(resource.resource_globs));
    TRY(encoder.encode(resource.matches));
    return {};
}

template<>
ErrorOr<Web::Extensions::WebAccessibleResourceDescriptor> decode(Decoder& decoder)
{
    Web::Extensions::WebAccessibleResourceDescriptor resource;
    resource.resource_globs = TRY(decoder.decode<Vector<String>>());
    resource.matches = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    return resource;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::ExtensionDescriptor const& extension)
{
    TRY(encoder.encode(extension.id));
    TRY(encoder.encode(extension.revision));
    TRY(encoder.encode(extension.name));
    TRY(encoder.encode(extension.version));
    TRY(encoder.encode(extension.manifest_version));
    TRY(encoder.encode(extension.manifest_json));
    TRY(encoder.encode(extension.extension_pages_csp));
    TRY(encoder.encode(extension.web_accessible_resources));
    TRY(encoder.encode(extension.content_scripts));
    TRY(encoder.encode(extension.has_background));
    TRY(encoder.encode(extension.background_document_html));
    return {};
}

template<>
ErrorOr<Web::Extensions::ExtensionDescriptor> decode(Decoder& decoder)
{
    Web::Extensions::ExtensionDescriptor extension;
    extension.id = TRY(decoder.decode<String>());
    extension.revision = TRY(decoder.decode<u64>());
    extension.name = TRY(decoder.decode<String>());
    extension.version = TRY(decoder.decode<String>());
    extension.manifest_version = TRY(decoder.decode<u8>());
    extension.manifest_json = TRY(decoder.decode<String>());
    extension.extension_pages_csp = TRY(decoder.decode<String>());
    extension.web_accessible_resources = TRY(decoder.decode<Vector<Web::Extensions::WebAccessibleResourceDescriptor>>());
    extension.content_scripts = TRY(decoder.decode<Vector<Web::Extensions::ContentScriptDescriptor>>());
    extension.has_background = TRY(decoder.decode<bool>());
    extension.background_document_html = TRY(decoder.decode<String>());
    return extension;
}

}
