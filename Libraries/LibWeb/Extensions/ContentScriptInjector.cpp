/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ContentScriptInjector.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/Extensions/ScriptExecutor.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

// The URL a content script's patterns are matched against: the document's own, except that an about:blank or
// about:srcdoc document stands in for the document that created it when the script opts in with match_about_blank.
static Optional<URL::URL> effective_url_for_matching(DOM::Document const& document, ContentScriptDescriptor const& script)
{
    auto const& url = document.url();
    if (!HTML::url_matches_about_blank(url) && !HTML::url_matches_about_srcdoc(url))
        return url;
    if (!script.match_about_blank)
        return {};
    return document.url_for_extension_matching();
}

bool content_script_matches(DOM::Document const& document, ContentScriptDescriptor const& script)
{
    auto browsing_context = document.browsing_context();
    if (!browsing_context)
        return false;
    if (!script.all_frames && !browsing_context->is_top_level())
        return false;

    auto url = effective_url_for_matching(document, script);
    if (!url.has_value())
        return false;

    for (auto const& pattern : script.exclude_matches) {
        if (pattern.matches(*url))
            return false;
    }
    for (auto const& pattern : script.matches) {
        if (pattern.matches(*url))
            return true;
    }
    return false;
}

static URL::URL script_url(ExtensionDescriptor const& extension, String const& path)
{
    StringBuilder url;
    url.append(scheme);
    url.append("://"sv);
    url.append(extension.id);
    url.append('/');
    url.append(path.bytes_as_string_view().trim("/"sv, TrimMode::Left));
    return URL::Parser::basic_parse(url.string_view()).value();
}

static void inject(DOM::Document& document, ExtensionDescriptor const& extension, ContentScriptDescriptor const& script)
{
    for (size_t i = 0; i < script.css_sources.size(); ++i)
        document.add_extension_style_sheet(Utf16String::from_utf8(script.css_sources[i]), script_url(extension, script.css_paths[i]), { extension.id });

    if (script.js_sources.is_empty())
        return;

    // A script of the page's own world runs like one of the page's, with the document's settings and realm.
    if (script.world == ExecutionWorld::Main) {
        auto& settings = HTML::relevant_settings_object(*document.window());
        for (size_t i = 0; i < script.js_sources.size(); ++i) {
            auto source = Utf16String::from_utf8(script.js_sources[i]);
            auto classic_script = HTML::ClassicScript::create(script.js_paths[i].to_byte_string(), source, settings, script_url(extension, script.js_paths[i]));
            (void)classic_script->run();
        }
        return;
    }

    auto& world = document.ensure_extension_world(extension);
    for (size_t i = 0; i < script.js_sources.size(); ++i) {
        auto source = Utf16String::from_utf8(script.js_sources[i]);
        (void)world.run_script(script.js_paths[i].to_byte_string(), source, script_url(extension, script.js_paths[i]));
    }
}

static void run_content_scripts_at(DOM::Document& document, RunAt run_at)
{
    for (auto const& extension : ExtensionSet::the().extensions()) {
        for (auto const& script : extension.content_scripts) {
            if (script.run_at != run_at || !content_script_matches(document, script))
                continue;
            inject(document, extension, script);
            // A script may navigate or destroy the document; nothing runs in one that is no longer active.
            if (!document.is_fully_active())
                return;
        }
    }
}

void run_content_scripts(DOM::Document& document, RunAt up_to)
{
    if (!document.is_fully_active() || !document.window() || ExtensionSet::the().extensions().is_empty())
        return;

    // Each point runs once per document, and a later point first catches up on any earlier one the document skipped:
    // an about:blank document has no parser to fire DOMContentLoaded, so its document_end scripts run at document_idle.
    for (auto run_at : { RunAt::DocumentStart, RunAt::DocumentEnd, RunAt::DocumentIdle }) {
        if (to_underlying(run_at) > to_underlying(up_to))
            break;
        if (document.content_scripts_ran_at(run_at))
            continue;
        document.set_content_scripts_ran_at(run_at);
        run_content_scripts_at(document, run_at);
        if (!document.is_fully_active())
            return;
        run_pending_injections(document, run_at);
        if (!document.is_fully_active())
            return;
    }
}

}
