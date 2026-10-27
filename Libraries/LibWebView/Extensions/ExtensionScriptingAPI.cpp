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

// A file names a path in the extension, relative to its root or from it.
static Optional<String> read_extension_file(Extension const& extension, String const& path)
{
    auto url_path = path.starts_with('/') ? path : MUST(String::formatted("/{}", path));
    auto file = extension.open_resource(url_path);
    if (file.is_error())
        return {};
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return {};
    auto source = String::from_utf8(bytes.value());
    if (source.is_error())
        return {};
    return source.release_value();
}

static String file_load_error(String const& path)
{
    return MUST(String::formatted("Could not load file: '{}'.", path));
}

struct ExtensionFile {
    String path;
    String source;
};

// Each file's path and source, in order.
static ErrorOr<Vector<ExtensionFile>, String> read_extension_files(Extension const& extension, JsonArray const& files)
{
    Vector<ExtensionFile> sources;
    for (auto const& path : files.values()) {
        if (!path.is_string())
            continue;
        auto source = read_extension_file(extension, path.as_string());
        if (!source.has_value())
            return file_load_error(path.as_string());
        sources.append({ path.as_string(), source.release_value() });
    }
    return sources;
}

static ErrorOr<ExtensionInjectionTarget, String> parse_injection_target(JsonObject const& details, StringView method)
{
    ExtensionInjectionTarget result;
    auto target = details.get_object("target"sv);
    if (!target.has_value() || !target->get_integer<i64>("tabId"sv).has_value())
        return "The injection needs a target tab"_string;
    result.tab_id = *target->get_integer<i64>("tabId"sv);
    result.all_frames = target->get_bool("allFrames"sv).value_or(false);
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
        result.frame_ids = move(ids);
    }
    if (result.all_frames && (result.frame_ids.has_value() || target->has("documentIds"sv)))
        return "Cannot specify 'allFrames' if either 'frameIds' or 'documentIds' is specified."_string;
    if (target->has("documentIds"sv))
        return MUST(String::formatted("scripting.{}() doesn't support documentIds yet", method));
    return result;
}

ErrorOr<ExtensionInjectionRequest, String> parse_extension_injection(Extension const& extension, JsonObject const& details)
{
    ExtensionInjectionRequest request;
    request.target = TRY(parse_injection_target(details, "executeScript"sv));

    auto function_source = details.get_string("func"sv);
    auto files = details.get_array("files"sv);
    if (function_source.has_value() == files.has_value())
        return "Exactly one of 'func' and 'files' must be specified"_string;
    if (function_source.has_value()) {
        request.injection.function_source = *function_source;
        request.injection.arguments_json = details.get_string("args"sv).value_or("[]"_string);
    } else {
        for (auto& file : TRY(read_extension_files(extension, *files))) {
            request.injection.js_paths.append(move(file.path));
            request.injection.js_sources.append(move(file.source));
        }
    }
    request.injection.world = details.get_string("world"sv) == "MAIN"sv ? Web::Extensions::ExecutionWorld::Main : Web::Extensions::ExecutionWorld::Isolated;
    // A script waits for the document to be idle, unless the call asks for it to run immediately.
    if (!details.get_bool("injectImmediately"sv).value_or(false))
        request.injection.run_at = Web::Extensions::RunAt::DocumentIdle;
    return request;
}

ErrorOr<ExtensionInjectionRequest, String> parse_extension_css_injection(Extension const& extension, JsonObject const& details, ExtensionCSSInjection kind)
{
    ExtensionInjectionRequest request;
    request.target = TRY(parse_injection_target(details, kind == ExtensionCSSInjection::Insert ? "insertCSS"sv : "removeCSS"sv));
    request.injection.remove_css = kind == ExtensionCSSInjection::Remove;
    auto css = details.get_string("css"sv);
    auto files = details.get_array("files"sv);
    if (css.has_value() == files.has_value())
        return "Exactly one of 'css' and 'files' must be specified."_string;
    // A sheet is known by its code or its file, so removeCSS() of a file's text doesn't remove the file's sheet.
    if (css.has_value()) {
        request.injection.css_sources.append(*css);
        request.injection.css_keys.append(MUST(String::formatted("code:{}", *css)));
    } else {
        for (auto& file : TRY(read_extension_files(extension, *files))) {
            request.injection.css_keys.append(MUST(String::formatted("file:{}", file.path)));
            request.injection.css_sources.append(move(file.source));
        }
    }
    request.injection.css_user_origin = details.get_string("origin"sv) == "USER"sv;
    return request;
}

ErrorOr<ExtensionInjectionRequest, String> parse_extension_tabs_injection(Extension const& extension, i64 tab_id, JsonObject const& details, ExtensionTabsInjection kind)
{
    ExtensionInjectionRequest request;
    request.target.tab_id = tab_id;
    request.target.all_frames = details.get_bool("allFrames"sv).value_or(false);
    request.target.match_about_blank = details.get_bool("matchAboutBlank"sv).value_or(false);
    request.target.root_error_rejects = true;
    // A frame id names the frame alone, or with allFrames, the root of the subtree to inject into.
    if (auto frame_id = details.get_integer<i64>("frameId"sv); frame_id.has_value()) {
        if (request.target.all_frames)
            request.target.root_frame_id = *frame_id;
        else
            request.target.frame_ids = Vector<i64> { *frame_id };
    }

    auto code = details.get_string("code"sv);
    auto file = details.get_string("file"sv);
    if (!code.has_value() && !file.has_value())
        return "No source code or file specified."_string;
    if (code.has_value() && file.has_value())
        return "Code and file should not be specified at the same time in the second argument."_string;
    auto source = code.has_value() ? Optional<String> { *code } : read_extension_file(extension, *file);
    if (!source.has_value())
        return file_load_error(*file);
    auto run_at = details.get_string("runAt"sv);
    request.injection.run_at = run_at == "document_start"sv ? Web::Extensions::RunAt::DocumentStart : run_at == "document_end"sv ? Web::Extensions::RunAt::DocumentEnd
                                                                                                                                 : Web::Extensions::RunAt::DocumentIdle;
    if (kind == ExtensionTabsInjection::CSS) {
        request.injection.css_keys.append(file.has_value() ? MUST(String::formatted("file:{}", *file)) : MUST(String::formatted("code:{}", *source)));
        request.injection.css_sources.append(source.release_value());
        request.injection.css_user_origin = details.get_string("cssOrigin"sv) == "user"sv;
    } else {
        request.injection.js_paths.append(file.value_or("code"_string));
        request.injection.js_sources.append(source.release_value());
    }
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
