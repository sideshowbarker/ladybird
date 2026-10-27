/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebCommon/HTML/BrowsingContext.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionScriptingAPI.h>

namespace WebView {

ErrorOr<ExtensionInjectionRequest, String> parse_extension_injection(Extension const& extension, JsonObject const& details)
{
    ExtensionInjectionRequest request;
    auto target = details.get_object("target"sv);
    if (!target.has_value() || !target->get_integer<i64>("tabId"sv).has_value())
        return "The injection needs a target tab"_string;
    request.target.tab_id = *target->get_integer<i64>("tabId"sv);
    request.target.all_frames = target->get_bool("allFrames"sv).value_or(false);
    if (target->has("frameIds"sv)) {
        auto frame_ids = target->get_array("frameIds"sv);
        if (!frame_ids.has_value())
            return "The target's frameIds must be an array"_string;
        // Each frame is injected into once, however often the call names it.
        Vector<i64> ids;
        for (auto const& id : frame_ids->values()) {
            auto value = id.get_integer<i64>();
            if (!value.has_value())
                return "The target's frameIds must be integers"_string;
            if (!ids.contains_slow(*value))
                ids.append(*value);
        }
        request.target.frame_ids = move(ids);
    }
    if (request.target.all_frames && (request.target.frame_ids.has_value() || target->has("documentIds"sv)))
        return "Cannot specify 'allFrames' if either 'frameIds' or 'documentIds' is specified."_string;
    if (target->has("documentIds"sv))
        return "scripting.executeScript() doesn't support documentIds yet"_string;

    auto function_source = details.get_string("func"sv);
    auto files = details.get_array("files"sv);
    if (function_source.has_value() == files.has_value())
        return "Exactly one of 'func' and 'files' must be specified"_string;
    if (function_source.has_value()) {
        request.injection.function_source = *function_source;
        request.injection.arguments_json = details.get_string("args"sv).value_or("[]"_string);
    } else {
        Optional<String> error;
        files->for_each([&](JsonValue const& path) {
            if (error.has_value() || !path.is_string())
                return;
            // A file names a path in the extension, relative to its root or from it.
            auto url_path = path.as_string().starts_with('/') ? path.as_string() : MUST(String::formatted("/{}", path.as_string()));
            auto file = extension.open_resource(url_path);
            auto bytes = file.is_error() ? ErrorOr<ByteBuffer> { file.release_error() } : file.value()->read_until_eof();
            auto source = bytes.is_error() ? ErrorOr<String> { bytes.release_error() } : String::from_utf8(bytes.value());
            if (source.is_error()) {
                error = MUST(String::formatted("Could not load file: '{}'.", path.as_string()));
                return;
            }
            request.injection.js_paths.append(path.as_string());
            request.injection.js_sources.append(source.release_value());
        });
        if (error.has_value())
            return error.release_value();
    }
    request.injection.world = details.get_string("world"sv) == "MAIN"sv ? Web::Extensions::ExecutionWorld::Main : Web::Extensions::ExecutionWorld::Isolated;
    // A script waits for the document to be idle, unless the call asks for it to run immediately.
    if (!details.get_bool("injectImmediately"sv).value_or(false))
        request.injection.run_at = Web::Extensions::RunAt::DocumentIdle;
    return request;
}

Optional<String> extension_injection_refusal(Extension const& extension, URL::URL const& url, bool may_see_url, bool has_active_tab)
{
    auto refusal_for_host = [&] {
        if (may_see_url)
            return MUST(String::formatted("Cannot access contents of url \"{}\". Extension manifest must request permission to access this host.", url.serialize()));
        return "Cannot access contents of the page. Extension manifest must request permission to access the respective host."_string;
    };
    if (url.scheme() == "about"sv && !Web::HTML::url_matches_about_blank(url) && !Web::HTML::url_matches_about_srcdoc(url))
        return refusal_for_host();
    if (url.scheme() == Web::Extensions::scheme) {
        if (url.serialized_host() == extension.id())
            return {};
        return "Cannot access a chrome-extension:// URL of different extension"_string;
    }
    if (has_active_tab || extension.descriptor().has_host_access(url))
        return {};
    return refusal_for_host();
}

}
