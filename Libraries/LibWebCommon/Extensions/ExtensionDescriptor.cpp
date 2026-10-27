/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/Extensions/ResourcePath.h>

namespace Web::Extensions {

bool ExtensionDescriptor::differs_only_at_runtime(ExtensionDescriptor const& other) const
{
    auto as_installed = [](ExtensionDescriptor descriptor) {
        descriptor.api_permissions.clear();
        descriptor.granted_origins.clear();
        descriptor.content_scripts.remove_all_matching([](auto const& script) { return script.id.has_value(); });
        return descriptor;
    };
    return as_installed(*this) == as_installed(other);
}

bool ExtensionDescriptor::has_host_access(URL::URL const& url) const
{
    auto matches = [&](auto const& pattern) { return pattern.matches(url); };
    return host_permissions.contains(matches) || granted_origins.contains(matches);
}

bool ExtensionDescriptor::is_web_accessible(StringView url_path, URL::URL const& initiator) const
{
    auto relative_path = canonical_resource_path(url_path);
    if (relative_path.is_error())
        return false;
    for (auto const& resource : web_accessible_resources) {
        bool path_allowed = false;
        for (auto const& glob : resource.resource_globs) {
            if (glob_matches(glob, relative_path.value())) {
                path_allowed = true;
                break;
            }
        }
        if (!path_allowed)
            continue;
        for (auto const& pattern : resource.matches) {
            if (pattern.matches(initiator))
                return true;
        }
    }
    return false;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::ContentScriptDescriptor const& script)
{
    TRY(encoder.encode(script.id));
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
    script.id = TRY(decoder.decode<Optional<String>>());
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
    TRY(encoder.encode(extension.messages));
    TRY(encoder.encode(extension.ui_locale));
    TRY(encoder.encode(extension.api_permissions));
    TRY(encoder.encode(extension.host_permissions));
    TRY(encoder.encode(extension.optional_api_permissions));
    TRY(encoder.encode(extension.optional_host_permissions));
    TRY(encoder.encode(extension.granted_origins));
    TRY(encoder.encode(extension.extension_pages_csp));
    TRY(encoder.encode(extension.web_accessible_resources));
    TRY(encoder.encode(extension.content_scripts));
    TRY(encoder.encode(extension.has_background));
    TRY(encoder.encode(extension.has_action));
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
    extension.messages = TRY((decoder.decode<HashMap<String, String>>()));
    extension.ui_locale = TRY(decoder.decode<String>());
    extension.api_permissions = TRY(decoder.decode<Vector<String>>());
    extension.host_permissions = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    extension.optional_api_permissions = TRY(decoder.decode<Vector<String>>());
    extension.optional_host_permissions = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    extension.granted_origins = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    extension.extension_pages_csp = TRY(decoder.decode<String>());
    extension.web_accessible_resources = TRY(decoder.decode<Vector<Web::Extensions::WebAccessibleResourceDescriptor>>());
    extension.content_scripts = TRY(decoder.decode<Vector<Web::Extensions::ContentScriptDescriptor>>());
    extension.has_background = TRY(decoder.decode<bool>());
    extension.has_action = TRY(decoder.decode<bool>());
    extension.background_document_html = TRY(decoder.decode<String>());
    return extension;
}

}
