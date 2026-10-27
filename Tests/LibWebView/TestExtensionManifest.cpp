/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionManifest.h>

using WebView::Extension;
using WebView::ExtensionManifest;
using namespace Web::Extensions;

static ExtensionManifest parse(StringView json)
{
    auto result = ExtensionManifest::parse(json);
    if (result.is_error())
        FAIL(result.error());
    return result.release_value();
}

static String parse_error(StringView json)
{
    auto result = ExtensionManifest::parse(json);
    VERIFY(result.is_error());
    return result.release_error();
}

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

TEST_CASE(parses_mv3_manifest_with_content_scripts)
{
    auto manifest = parse(R"({
        "manifest_version": 3,
        "name": "Example",
        "version": "1.0",
        "description": "An example",
        "content_scripts": [
            {
                "matches": ["https://example.com/*", "<all_urls>"],
                "exclude_matches": ["https://example.com/private/*"],
                "js": ["a.js", "b.js"],
                "css": ["style.css"],
                "run_at": "document_start",
                "all_frames": true,
                "match_about_blank": true,
                "world": "MAIN"
            },
            { "matches": ["*://*/*"], "js": ["c.js"] }
        ]
    })"sv);

    EXPECT_EQ(manifest.manifest_version, 3);
    EXPECT_EQ(manifest.name, "Example"sv);
    EXPECT_EQ(manifest.version, "1.0"sv);
    EXPECT_EQ(manifest.description, "An example"sv);
    EXPECT_EQ(manifest.extension_pages_csp, "script-src 'self'; object-src 'self'"sv);
    EXPECT(!manifest.background.has_value());

    EXPECT_EQ(manifest.content_scripts.size(), 2u);
    auto const& first = manifest.content_scripts[0];
    EXPECT_EQ(first.matches.size(), 2u);
    EXPECT_EQ(first.matches[0], MUST(MatchPattern::parse("https://example.com/*"sv)));
    EXPECT(first.matches[1].matches_all_urls());
    EXPECT_EQ(first.exclude_matches.size(), 1u);
    EXPECT_EQ(first.js_paths, (Vector { "a.js"_string, "b.js"_string }));
    EXPECT(first.js_sources.is_empty());
    EXPECT_EQ(first.css_paths, (Vector { "style.css"_string }));
    EXPECT_EQ(first.run_at, RunAt::DocumentStart);
    EXPECT(first.all_frames);
    EXPECT(first.match_about_blank);
    EXPECT_EQ(first.world, ExecutionWorld::Main);

    auto const& second = manifest.content_scripts[1];
    EXPECT_EQ(second.run_at, RunAt::DocumentIdle);
    EXPECT(!second.all_frames);
    EXPECT(!second.match_about_blank);
    EXPECT_EQ(second.world, ExecutionWorld::Isolated);
    EXPECT(second.css_paths.is_empty());
}

TEST_CASE(parses_mv2_manifest)
{
    auto manifest = parse(R"({
        "manifest_version": 2,
        "name": "Old",
        "version": "0.1",
        "content_security_policy": "script-src 'self' 'unsafe-eval'; object-src 'self'",
        "web_accessible_resources": ["images/*.png", "page.html"],
        "background": { "scripts": ["bg1.js", "bg2.js"] },
        "permissions": ["storage", "https://api.example.com/*"]
    })"sv);

    EXPECT_EQ(manifest.manifest_version, 2);
    EXPECT_EQ(manifest.extension_pages_csp, "script-src 'self' 'unsafe-eval'; object-src 'self'"sv);
    EXPECT_EQ(manifest.web_accessible_resources.size(), 1u);
    EXPECT_EQ(manifest.web_accessible_resources[0].resource_globs, (Vector { "images/*.png"_string, "page.html"_string }));
    EXPECT_EQ(manifest.web_accessible_resources[0].matches, (Vector { MatchPattern::all_urls() }));
    EXPECT(manifest.background.has_value());
    EXPECT_EQ(manifest.background->scripts, (Vector { "bg1.js"_string, "bg2.js"_string }));
    EXPECT_EQ(manifest.permissions, (Vector { "storage"_string }));
    EXPECT_EQ(manifest.host_permissions, (Vector { "https://api.example.com/*"_string }));
}

TEST_CASE(parses_mv3_web_accessible_resources)
{
    auto manifest = parse(R"({
        "manifest_version": 3,
        "name": "WAR",
        "version": "1",
        "web_accessible_resources": [
            { "resources": ["images/*"], "matches": ["https://example.com/*"] },
            { "resources": ["fonts/*"], "matches": ["<all_urls>"] }
        ],
        "host_permissions": ["https://example.com/*"],
        "permissions": ["tabs"]
    })"sv);

    EXPECT_EQ(manifest.web_accessible_resources.size(), 2u);
    EXPECT_EQ(manifest.web_accessible_resources[0].resource_globs, (Vector { "images/*"_string }));
    EXPECT_EQ(manifest.web_accessible_resources[0].matches, (Vector { MUST(MatchPattern::parse("https://example.com/*"sv)) }));
    EXPECT(manifest.web_accessible_resources[1].matches[0].matches_all_urls());
    EXPECT_EQ(manifest.host_permissions, (Vector { "https://example.com/*"_string }));
    EXPECT_EQ(manifest.permissions, (Vector { "tabs"_string }));
}

TEST_CASE(invalid_match_pattern_is_an_error)
{
    auto error = parse_error(R"({
        "manifest_version": 3, "name": "Bad", "version": "1",
        "content_scripts": [{ "matches": ["https://*foo/"], "js": ["a.js"] }]
    })"sv);
    EXPECT(error.contains("content_scripts"sv));
    EXPECT(error.contains("https://*foo/"sv));
}

TEST_CASE(missing_required_keys_are_errors)
{
    EXPECT(parse_error(R"({ "name": "x", "version": "1" })"sv).contains("manifest_version"sv));
    EXPECT(parse_error(R"({ "manifest_version": 3, "version": "1" })"sv).contains("name"sv));
    EXPECT(parse_error(R"({ "manifest_version": 3, "name": "x" })"sv).contains("version"sv));
    EXPECT(parse_error(R"({ "manifest_version": 4, "name": "x", "version": "1" })"sv).contains("manifest_version"sv));
    EXPECT(parse_error(R"({ "manifest_version": 3, "name": "x", "version": "1", "content_scripts": [{ "js": ["a.js"] }] })"sv).contains("matches"sv));
    EXPECT(parse_error("not json"sv).contains("JSON"sv));
    EXPECT(parse_error("[]"sv).contains("object"sv));
}

TEST_CASE(background_variants)
{
    auto scripts = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "scripts": ["a.js"] } })"sv);
    EXPECT_EQ(scripts.background->scripts, (Vector { "a.js"_string }));
    EXPECT(!scripts.background->page.has_value());
    EXPECT(!scripts.background->service_worker.has_value());
    EXPECT(!scripts.background->is_module);

    auto page = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "page": "bg.html" } })"sv);
    EXPECT_EQ(page.background->page, "bg.html"sv);

    auto worker = parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "background": { "service_worker": "sw.js", "type": "module" } })"sv);
    EXPECT_EQ(worker.background->service_worker, "sw.js"sv);
    EXPECT(worker.background->is_module);

    auto none = parse(R"({ "manifest_version": 3, "name": "x", "version": "1" })"sv);
    EXPECT(!none.background.has_value());
}

TEST_CASE(background_persistence)
{
    auto mv2 = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "scripts": ["a.js"] } })"sv);
    EXPECT(mv2.background->persistent);
    auto event_page = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "scripts": ["a.js"], "persistent": false } })"sv);
    EXPECT(!event_page.background->persistent);
    auto mv3 = parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "background": { "scripts": ["a.js"] } })"sv);
    EXPECT(!mv3.background->persistent);
    // Manifest version 3 has no persistent pages, so the key is ignored there.
    auto mv3_persistent = parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "background": { "scripts": ["a.js"], "persistent": true } })"sv);
    EXPECT(!mv3_persistent.background->persistent);
    EXPECT(parse_error(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "scripts": ["a.js"], "persistent": "no" } })"sv).contains("persistent"sv));
}

TEST_CASE(generated_background_page)
{
    auto scripts = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "scripts": ["a.js", "sub/b.js"] } })"sv);
    auto html = scripts.generated_background_document_html();
    EXPECT(html.contains("<script src=\"/a.js\"></script>"sv));
    EXPECT(html.contains("<script src=\"/sub/b.js\"></script>"sv));
    EXPECT(html.starts_with_bytes("<!DOCTYPE html>"sv));
    // The scripts run in the body, so that it exists while they run.
    auto body = html.find_byte_offset("<body>"sv);
    auto first_script = html.find_byte_offset("<script"sv);
    VERIFY(body.has_value() && first_script.has_value());
    EXPECT(*first_script > *body);

    auto worker = parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "background": { "service_worker": "sw.js", "type": "module" } })"sv);
    EXPECT(worker.generated_background_document_html().contains("<script type=\"module\" src=\"/sw.js\"></script>"sv));

    auto page = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "background": { "page": "bg.html" } })"sv);
    EXPECT(page.generated_background_document_html().is_empty());
}

namespace {

struct TemporaryExtension {
    TemporaryExtension()
    {
        path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-extension-manifest-{}-{}", Core::System::getpid(), ++s_counter)).string();
        MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
    }

    ~TemporaryExtension()
    {
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Allowed));
    }

    void write(StringView relative_path, StringView contents)
    {
        auto full_path = LexicalPath::join(path, relative_path).string();
        MUST(Core::Directory::create(LexicalPath(full_path).dirname(), Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(full_path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents.bytes()));
    }

    ByteString path;
    static inline int s_counter = 0;
};

}

static constexpr auto test_id = "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"sv;

TEST_CASE(load_unpacked_reads_sources)
{
    TemporaryExtension directory;
    directory.write("manifest.json"sv, R"({
        "manifest_version": 3, "name": "Loaded", "version": "2",
        "content_scripts": [{ "matches": ["<all_urls>"], "js": ["a.js"], "css": ["b.css"] }],
        "background": { "scripts": ["bg.js"] }
    })"sv);
    directory.write("a.js"sv, "console.log('a');"sv);
    directory.write("b.css"sv, "p { color: red }"sv);

    auto extension = TRY_OR_FAIL(Extension::load_unpacked(directory.path, MUST(String::from_utf8(test_id))));
    EXPECT_EQ(extension->id(), test_id);
    EXPECT_EQ(extension->directory(), directory.path);
    EXPECT_EQ(extension->manifest().name, "Loaded"sv);

    auto const& descriptor = extension->descriptor();
    EXPECT_EQ(descriptor.id, test_id);
    EXPECT_EQ(descriptor.name, "Loaded"sv);
    EXPECT_EQ(descriptor.version, "2"sv);
    EXPECT_EQ(descriptor.manifest_version, 3);
    EXPECT(descriptor.manifest_json.contains("\"name\""sv));
    EXPECT_EQ(descriptor.content_scripts.size(), 1u);
    EXPECT_EQ(descriptor.content_scripts[0].js_paths, (Vector { "a.js"_string }));
    EXPECT_EQ(descriptor.content_scripts[0].js_sources, (Vector { "console.log('a');"_string }));
    EXPECT_EQ(descriptor.content_scripts[0].css_sources, (Vector { "p { color: red }"_string }));
    EXPECT(descriptor.has_background);
    EXPECT(descriptor.background_document_html.contains("/bg.js"sv));
}

TEST_CASE(load_unpacked_reports_missing_files)
{
    TemporaryExtension directory;
    directory.write("manifest.json"sv, R"({
        "manifest_version": 3, "name": "Missing", "version": "1",
        "content_scripts": [{ "matches": ["<all_urls>"], "js": ["gone.js"] }]
    })"sv);

    auto result = Extension::load_unpacked(directory.path, MUST(String::from_utf8(test_id)));
    EXPECT(result.is_error());
    EXPECT(result.error().contains("gone.js"sv));

    auto no_manifest = Extension::load_unpacked(LexicalPath::join(directory.path, "nope"sv).string(), MUST(String::from_utf8(test_id)));
    EXPECT(no_manifest.is_error());
    EXPECT(no_manifest.error().contains("manifest.json"sv));
}

TEST_CASE(manifest_sources_cannot_leave_the_extension_directory)
{
    TemporaryExtension directory;
    directory.write("outside.js"sv, "throw new Error('outside');"sv);
    directory.write("outside.css"sv, "body { color: red }"sv);
    auto package = LexicalPath::join(directory.path, "package"sv).string();
    directory.write("package/manifest.json"sv, R"({
        "manifest_version": 3, "name": "Paths", "version": "1",
        "content_scripts": [{ "matches": ["<all_urls>"], "js": ["../outside.js"] }]
    })"sv);
    EXPECT(Extension::load_unpacked(package, MUST(String::from_utf8(test_id))).is_error());

    directory.write("package/manifest.json"sv, R"({
        "manifest_version": 3, "name": "Paths", "version": "1",
        "content_scripts": [{ "matches": ["<all_urls>"], "css": ["../outside.css"] }]
    })"sv);
    EXPECT(Extension::load_unpacked(package, MUST(String::from_utf8(test_id))).is_error());
}

#ifndef AK_OS_WINDOWS
TEST_CASE(resource_opener_checks_the_symlink_target)
{
    TemporaryExtension directory;
    directory.write("outside/secret.txt"sv, "outside"sv);
    directory.write("package/manifest.json"sv, R"({ "manifest_version": 3, "name": "Paths", "version": "1" })"sv);
    directory.write("package/public.txt"sv, "inside"sv);
    auto package = LexicalPath::join(directory.path, "package"sv).string();
    MUST(Core::System::symlink("../outside"sv, LexicalPath::join(package, "external"sv).string()));
    MUST(Core::System::symlink("public.txt"sv, LexicalPath::join(package, "internal"sv).string()));
    auto extension = TRY_OR_FAIL(Extension::load_unpacked(package, MUST(String::from_utf8(test_id))));
    EXPECT(extension->open_resource("/external/secret.txt"sv).is_error());
    auto internal = TRY_OR_FAIL(extension->open_resource("/internal"sv));
    auto contents = TRY_OR_FAIL(internal->read_until_eof());
    EXPECT_EQ(StringView(contents), "inside"sv);
}

TEST_CASE(manifest_sources_cannot_follow_external_symlinks)
{
    TemporaryExtension directory;
    directory.write("outside/source.js"sv, "throw new Error('outside');"sv);
    directory.write("package/manifest.json"sv, R"({
        "manifest_version": 3, "name": "Paths", "version": "1",
        "content_scripts": [{ "matches": ["<all_urls>"], "js": ["link/source.js"] }]
    })"sv);
    auto package = LexicalPath::join(directory.path, "package"sv).string();
    MUST(Core::System::symlink("../outside"sv, LexicalPath::join(package, "link"sv).string()));
    EXPECT(Extension::load_unpacked(package, MUST(String::from_utf8(test_id))).is_error());

    directory.write("package/manifest.json"sv, R"({
        "manifest_version": 3, "name": "Paths", "version": "1",
        "content_scripts": [{ "matches": ["<all_urls>"], "css": ["link/source.js"] }]
    })"sv);
    EXPECT(Extension::load_unpacked(package, MUST(String::from_utf8(test_id))).is_error());
}
#endif

TEST_CASE(resource_path_cannot_escape_the_extension_directory)
{
    TemporaryExtension directory;
    directory.write("manifest.json"sv, R"({ "manifest_version": 3, "name": "Paths", "version": "1" })"sv);
    directory.write("a/b.txt"sv, "b"sv);
    auto extension = TRY_OR_FAIL(Extension::load_unpacked(directory.path, MUST(String::from_utf8(test_id))));

    EXPECT_EQ(TRY_OR_FAIL(extension->resolve_resource_path("/manifest.json"sv)), LexicalPath::join(directory.path, "manifest.json"sv).string());
    EXPECT_EQ(TRY_OR_FAIL(extension->resolve_resource_path("/a/b.txt"sv)), LexicalPath::join(directory.path, "a/b.txt"sv).string());
    EXPECT_EQ(TRY_OR_FAIL(extension->resolve_resource_path("/a/../manifest.json"sv)), LexicalPath::join(directory.path, "manifest.json"sv).string());
    EXPECT_EQ(TRY_OR_FAIL(extension->resolve_resource_path("/a/%62.txt"sv)), LexicalPath::join(directory.path, "a/b.txt"sv).string());

    EXPECT(extension->resolve_resource_path("/../manifest.json"sv).is_error());
    EXPECT(extension->resolve_resource_path("/a/../../x"sv).is_error());
    EXPECT(extension->resolve_resource_path("//etc/passwd"sv).is_error());
    EXPECT(extension->resolve_resource_path("/a/%2e%2e/%2e%2e/x"sv).is_error());
    EXPECT(extension->resolve_resource_path("relative"sv).is_error());
    EXPECT(extension->resolve_resource_path("/a/b.txt/../../../x"sv).is_error());
    EXPECT(extension->resolve_resource_path("/public/..%2fmanifest.json"sv).is_error());
    EXPECT(extension->resolve_resource_path("/public/..%2Fmanifest.json"sv).is_error());
    EXPECT(extension->resolve_resource_path("/public/..%5cmanifest.json"sv).is_error());
    EXPECT(extension->resolve_resource_path("/manifest.json%00.txt"sv).is_error());
}

TEST_CASE(web_accessible_resource_matching)
{
    TemporaryExtension directory;
    directory.write("manifest.json"sv, R"({
        "manifest_version": 3, "name": "WAR", "version": "1",
        "web_accessible_resources": [
            { "resources": ["images/*.png"], "matches": ["https://e.com/*"] },
            { "resources": ["public/*"], "matches": ["<all_urls>"] }
        ]
    })"sv);
    auto extension = TRY_OR_FAIL(Extension::load_unpacked(directory.path, MUST(String::from_utf8(test_id))));

    EXPECT(extension->is_web_accessible("/images/a.png"sv, url("https://e.com/page"sv)));
    EXPECT(extension->is_web_accessible("/images/sub/a.png"sv, url("https://e.com/page"sv)));
    EXPECT(!extension->is_web_accessible("/images/a.png"sv, url("https://other.com/"sv)));
    EXPECT(!extension->is_web_accessible("/images/a.jpg"sv, url("https://e.com/"sv)));
    EXPECT(extension->is_web_accessible("/public/anything.txt"sv, url("https://other.com/"sv)));
    EXPECT(!extension->is_web_accessible("/manifest.json"sv, url("https://e.com/"sv)));
    EXPECT(!extension->is_web_accessible("/public/..%2fmanifest.json"sv, url("https://e.com/"sv)));
    EXPECT(!extension->is_web_accessible("/public/..%5Cmanifest.json"sv, url("https://e.com/"sv)));
    EXPECT(!extension->is_web_accessible("/public/../manifest.json"sv, url("https://e.com/"sv)));
    EXPECT(!extension->is_web_accessible("/public/a.txt%00.js"sv, url("https://e.com/"sv)));
    EXPECT(extension->is_web_accessible("/images/%61.png"sv, url("https://e.com/"sv)));

    TemporaryExtension empty_directory;
    empty_directory.write("manifest.json"sv, R"({ "manifest_version": 3, "name": "None", "version": "1" })"sv);
    auto locked = TRY_OR_FAIL(Extension::load_unpacked(empty_directory.path, MUST(String::from_utf8(test_id))));
    EXPECT(!locked->is_web_accessible("/anything"sv, url("https://e.com/"sv)));
}

TEST_CASE(malformed_match_host_reports_the_manifest_key)
{
    auto result = ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Invalid host", "version": "1",
        "content_scripts": [{ "matches": ["https://bad host/*"], "js": ["a.js"] }]
    })"sv);
    EXPECT(result.is_error());
    if (result.is_error())
        EXPECT(result.error().contains("matches"sv));
}

TEST_CASE(only_supported_api_permissions_are_retained)
{
    auto manifest = TRY_OR_FAIL(WebView::ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Permissions", "version": "1",
        "permissions": ["unknownPermission", "unlimitedStorage", "storage", "tabs"]
    })"sv));
    EXPECT_EQ(manifest.permissions.size(), 3u);
    EXPECT(!manifest.permissions.contains_slow("unknownPermission"sv));
}

TEST_CASE(manifest_action)
{
    auto mv3 = parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "action": { "default_popup": "/popup.html", "default_title": "Title", "default_icon": { "32": "icons/32.png", "16": "/icons/16.png" } } })"sv);
    VERIFY(mv3.action.has_value());
    EXPECT(!mv3.action->is_browser_action);
    EXPECT_EQ(mv3.action->default_popup, "popup.html"sv);
    EXPECT_EQ(mv3.action->default_title, "Title"sv);
    VERIFY(mv3.action->default_icon.size() == 2);
    EXPECT_EQ(mv3.action->default_icon[0].size, 16u);
    EXPECT_EQ(mv3.action->default_icon[0].path, "icons/16.png"sv);
    EXPECT_EQ(mv3.action->default_icon[1].size, 32u);

    auto mv2 = parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "browser_action": { "default_icon": "icon.png" } })"sv);
    VERIFY(mv2.action.has_value());
    EXPECT(mv2.action->is_browser_action);
    EXPECT(!mv2.action->default_popup.has_value());
    VERIFY(mv2.action->default_icon.size() == 1);
    EXPECT_EQ(mv2.action->default_icon[0].size, 0u);
    EXPECT_EQ(mv2.action->default_icon[0].path, "icon.png"sv);

    // Each manifest version reads only its own key.
    EXPECT(!parse(R"({ "manifest_version": 3, "name": "x", "version": "1", "browser_action": {} })"sv).action.has_value());
    EXPECT(!parse(R"({ "manifest_version": 2, "name": "x", "version": "1", "action": {} })"sv).action.has_value());
    EXPECT(!parse(R"({ "manifest_version": 3, "name": "x", "version": "1" })"sv).action.has_value());

    EXPECT(parse_error(R"({ "manifest_version": 3, "name": "x", "version": "1", "action": [] })"sv).contains("action"sv));
    EXPECT(parse_error(R"({ "manifest_version": 3, "name": "x", "version": "1", "action": { "default_popup": 1 } })"sv).contains("action.default_popup"sv));
    EXPECT(parse_error(R"({ "manifest_version": 2, "name": "x", "version": "1", "browser_action": { "default_icon": { "16": 1 } } })"sv).contains("browser_action.default_icon"sv));
}

TEST_CASE(optional_permissions_parse_in_both_manifest_versions)
{
    auto mv3 = TRY_OR_FAIL(WebView::ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Optional", "version": "1",
        "optional_permissions": ["tabs", "unknownPermission"],
        "optional_host_permissions": ["https://a.example/*"]
    })"sv));
    EXPECT_EQ(mv3.optional_permissions, (Vector { "tabs"_string }));
    EXPECT_EQ(mv3.optional_host_permissions, (Vector { "https://a.example/*"_string }));

    // Manifest version 2 mixes host patterns into "optional_permissions", as it does into "permissions".
    auto mv2 = TRY_OR_FAIL(WebView::ExtensionManifest::parse(R"({
        "manifest_version": 2, "name": "Optional", "version": "1",
        "optional_permissions": ["tabs", "https://b.example/*", "<all_urls>"]
    })"sv));
    EXPECT_EQ(mv2.optional_permissions, (Vector { "tabs"_string }));
    EXPECT_EQ(mv2.optional_host_permissions, (Vector { "https://b.example/*"_string, "<all_urls>"_string }));
}

TEST_CASE(commands_parse)
{
    auto manifest = TRY_OR_FAIL(WebView::ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Commands", "version": "1",
        "permissions": ["contextMenus", "menus"],
        "commands": {
            "toggle": { "description": "Toggle", "suggested_key": { "default": "Alt + Shift + D", "mac": "MacCtrl+Shift+D" } },
            "_execute_action": { "suggested_key": { "default": "Ctrl+Shift+Y" } },
            "quiet": { "description": "No shortcut" }
        }
    })"sv));
    // Both menu permissions are known, and "menus" is Firefox's name for "contextMenus".
    EXPECT_EQ(manifest.permissions, (Vector { "contextMenus"_string, "menus"_string }));
    EXPECT_EQ(manifest.commands.size(), 3u);
    auto const& toggle = manifest.commands[0];
    EXPECT_EQ(toggle.name, "toggle"sv);
    EXPECT_EQ(toggle.description, "Toggle"sv);
    // The platform's suggested key, or else the default one, with its spaces taken out.
#if defined(AK_OS_MACOS)
    EXPECT_EQ(toggle.shortcut, "MacCtrl+Shift+D"sv);
#else
    EXPECT_EQ(toggle.shortcut, "Alt+Shift+D"sv);
#endif
    EXPECT_EQ(manifest.commands[1].name, "_execute_action"sv);
    EXPECT_EQ(manifest.commands[1].shortcut, "Ctrl+Shift+Y"sv);
    EXPECT(!manifest.commands[2].shortcut.has_value());

    auto not_an_object = WebView::ExtensionManifest::parse(R"({ "manifest_version": 3, "name": "C", "version": "1", "commands": [] })"sv);
    EXPECT(not_an_object.is_error());
}
