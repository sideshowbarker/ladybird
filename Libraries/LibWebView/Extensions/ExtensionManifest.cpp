/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibWebView/Extensions/ExtensionManifest.h>

namespace WebView {

using Web::Extensions::ContentScriptDescriptor;
using Web::Extensions::ExecutionWorld;
using Web::Extensions::MatchPattern;
using Web::Extensions::RunAt;
using Web::Extensions::WebAccessibleResourceDescriptor;

// The policy an extension document gets when its manifest sets none.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/manifest.json/content_security_policy
static constexpr auto default_extension_pages_csp = "script-src 'self'; object-src 'self'"sv;

static String error_for(StringView key, StringView message)
{
    return MUST(String::formatted("manifest.json: \"{}\" {}", key, message));
}

static ErrorOr<Vector<String>, String> parse_string_array(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    auto array = object.get_array(key);
    if (!array.has_value()) {
        if (object.has(key))
            return error_for(key, "must be an array of strings"sv);
        return strings;
    }
    for (auto const& value : array->values()) {
        if (!value.is_string())
            return error_for(key, "must be an array of strings"sv);
        strings.append(value.as_string());
    }
    return strings;
}

static ErrorOr<Vector<MatchPattern>, String> parse_match_patterns(JsonObject const& object, StringView key, StringView parent_key)
{
    Vector<MatchPattern> patterns;
    for (auto const& pattern_string : TRY(parse_string_array(object, key))) {
        auto pattern = MatchPattern::parse(pattern_string);
        if (pattern.is_error())
            return MUST(String::formatted("manifest.json: \"{}.{}\": \"{}\" is not a valid match pattern: {}", parent_key, key, pattern_string, pattern.error()));
        patterns.append(pattern.release_value());
    }
    return patterns;
}

static ErrorOr<ContentScriptDescriptor, String> parse_content_script(JsonValue const& value)
{
    static constexpr auto key = "content_scripts"sv;
    if (!value.is_object())
        return error_for(key, "must be an array of objects"sv);
    auto const& object = value.as_object();

    ContentScriptDescriptor script;
    script.matches = TRY(parse_match_patterns(object, "matches"sv, key));
    if (script.matches.is_empty())
        return error_for(key, "entries need a non-empty \"matches\" array"sv);
    script.exclude_matches = TRY(parse_match_patterns(object, "exclude_matches"sv, key));
    script.js_paths = TRY(parse_string_array(object, "js"sv));
    script.css_paths = TRY(parse_string_array(object, "css"sv));

    if (auto run_at = object.get_string("run_at"sv); run_at.has_value()) {
        if (*run_at == "document_start"sv)
            script.run_at = RunAt::DocumentStart;
        else if (*run_at == "document_end"sv)
            script.run_at = RunAt::DocumentEnd;
        else if (*run_at == "document_idle"sv)
            script.run_at = RunAt::DocumentIdle;
        else
            return error_for(key, "has a \"run_at\" that is not document_start, document_end, or document_idle"sv);
    }

    script.all_frames = object.get_bool("all_frames"sv).value_or(false);
    script.match_about_blank = object.get_bool("match_about_blank"sv).value_or(false);

    if (auto world = object.get_string("world"sv); world.has_value()) {
        if (*world == "MAIN"sv)
            script.world = ExecutionWorld::Main;
        else if (*world == "ISOLATED"sv)
            script.world = ExecutionWorld::Isolated;
        else
            return error_for(key, "has a \"world\" that is not ISOLATED or MAIN"sv);
    }

    return script;
}

static ErrorOr<Vector<WebAccessibleResourceDescriptor>, String> parse_web_accessible_resources(JsonObject const& manifest)
{
    static constexpr auto key = "web_accessible_resources"sv;
    Vector<WebAccessibleResourceDescriptor> resources;
    auto array = manifest.get_array(key);
    if (!array.has_value()) {
        if (manifest.has(key))
            return error_for(key, "must be an array"sv);
        return resources;
    }
    if (array->size() == 0)
        return resources;

    // Manifest version 2 lists the resource paths directly, and every page may load them.
    if (array->at(0).is_string()) {
        WebAccessibleResourceDescriptor resource;
        resource.resource_globs = TRY(parse_string_array(manifest, key));
        resource.matches.append(MatchPattern::all_urls());
        resources.append(move(resource));
        return resources;
    }

    // Manifest version 3 lists objects, each pairing resource paths with the page URL patterns allowed to load them.
    for (auto const& value : array->values()) {
        if (!value.is_object())
            return error_for(key, "must be an array of strings or of objects"sv);
        auto const& object = value.as_object();
        WebAccessibleResourceDescriptor resource;
        resource.resource_globs = TRY(parse_string_array(object, "resources"sv));
        resource.matches = TRY(parse_match_patterns(object, "matches"sv, key));
        resources.append(move(resource));
    }
    return resources;
}

static ErrorOr<Optional<ExtensionManifest::Background>, String> parse_background(JsonObject const& manifest)
{
    static constexpr auto key = "background"sv;
    auto object = manifest.get_object(key);
    if (!object.has_value()) {
        if (manifest.has(key))
            return error_for(key, "must be an object"sv);
        return OptionalNone {};
    }

    ExtensionManifest::Background background;
    background.scripts = TRY(parse_string_array(*object, "scripts"sv));
    if (auto page = object->get_string("page"sv); page.has_value())
        background.page = *page;
    if (auto service_worker = object->get_string("service_worker"sv); service_worker.has_value())
        background.service_worker = *service_worker;
    auto type = object->get_string("type"sv);
    background.is_module = type.has_value() && *type == "module"sv;
    return background;
}

// Returns an error message, or nothing when the policy was read into the out parameter.
static Optional<String> parse_content_security_policy(JsonObject const& manifest, u8 manifest_version, String& extension_pages_csp)
{
    static constexpr auto key = "content_security_policy"sv;
    extension_pages_csp = MUST(String::from_utf8(default_extension_pages_csp));
    if (!manifest.has(key))
        return {};

    // Manifest version 2 holds the policy string itself; version 3 holds an object with an "extension_pages" entry.
    if (auto policy = manifest.get_string(key); policy.has_value()) {
        if (manifest_version != 2)
            return error_for(key, "must be an object with an \"extension_pages\" entry"sv);
        extension_pages_csp = *policy;
        return {};
    }
    if (auto object = manifest.get_object(key); object.has_value()) {
        if (auto policy = object->get_string("extension_pages"sv); policy.has_value())
            extension_pages_csp = *policy;
        return {};
    }
    return error_for(key, "must be a string or an object"sv);
}

ErrorOr<ExtensionManifest, String> ExtensionManifest::parse(StringView manifest_json)
{
    auto json = JsonValue::from_string(manifest_json);
    if (json.is_error())
        return "manifest.json: not valid JSON"_string;
    if (!json.value().is_object())
        return "manifest.json: the top level must be an object"_string;
    auto const& object = json.value().as_object();

    ExtensionManifest manifest;

    auto manifest_version = object.get_integer<u8>("manifest_version"sv);
    if (!manifest_version.has_value() || (*manifest_version != 2 && *manifest_version != 3))
        return error_for("manifest_version"sv, "must be 2 or 3"sv);
    manifest.manifest_version = *manifest_version;

    auto name = object.get_string("name"sv);
    if (!name.has_value() || name->is_empty())
        return error_for("name"sv, "is required"sv);
    manifest.name = *name;

    auto version = object.get_string("version"sv);
    if (!version.has_value() || version->is_empty())
        return error_for("version"sv, "is required"sv);
    manifest.version = *version;

    manifest.description = object.get_string("description"sv).value_or({});
    manifest.default_locale = object.get_string("default_locale"sv).value_or({});

    if (auto scripts = object.get_array("content_scripts"sv); scripts.has_value()) {
        for (auto const& value : scripts->values())
            manifest.content_scripts.append(TRY(parse_content_script(value)));
    } else if (object.has("content_scripts"sv)) {
        return error_for("content_scripts"sv, "must be an array"sv);
    }

    manifest.web_accessible_resources = TRY(parse_web_accessible_resources(object));

    // Manifest version 2 mixes host patterns into "permissions"; version 3 keeps them in "host_permissions".
    for (auto const& permission : TRY(parse_string_array(object, "permissions"sv))) {
        if (permission == "<all_urls>"sv || permission.contains("://"sv))
            manifest.host_permissions.append(permission);
        else
            manifest.permissions.append(permission);
    }
    manifest.host_permissions.extend(TRY(parse_string_array(object, "host_permissions"sv)));

    if (auto error = parse_content_security_policy(object, manifest.manifest_version, manifest.extension_pages_csp); error.has_value())
        return error.release_value();
    manifest.background = TRY(parse_background(object));

    return manifest;
}

String ExtensionManifest::generated_background_document_html() const
{
    if (!background.has_value() || background->page.has_value())
        return {};

    Vector<String> script_paths = background->scripts;
    if (script_paths.is_empty() && background->service_worker.has_value())
        script_paths.append(*background->service_worker);
    if (script_paths.is_empty())
        return {};

    StringBuilder html;
    html.append("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"sv);
    for (auto const& path : script_paths) {
        html.append("<script "sv);
        if (background->is_module)
            html.append("type=\"module\" "sv);
        html.append("src=\"/"sv);
        html.append(path.bytes_as_string_view().trim("/"sv, TrimMode::Left));
        html.append("\"></script>"sv);
    }
    html.append("</head><body></body></html>"sv);
    return MUST(html.to_string());
}

}
