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
#include <LibWebView/Extensions/ExtensionContentScripts.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>

using Web::Extensions::ContentScriptDescriptor;
using WebView::ExtensionContentScriptRegistrations;

namespace {

struct TemporaryDirectory {
    TemporaryDirectory()
    {
        path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-extension-content-scripts-{}-{}", Core::System::getpid(), ++s_counter)).string();
        MUST(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
        path = MUST(FileSystem::real_path(path));
    }

    ~TemporaryDirectory()
    {
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Allowed));
    }

    ByteString extension(StringView name, StringView manifest_json)
    {
        auto directory = LexicalPath::join(path, name).string();
        MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(LexicalPath::join(directory, "manifest.json"sv).string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(manifest_json.bytes()));
        return directory;
    }

    ByteString path;
    static inline int s_counter = 0;
};

}

static ContentScriptDescriptor script(StringView source)
{
    ContentScriptDescriptor script;
    script.matches = { Web::Extensions::MatchPattern::all_urls() };
    script.js_paths = { "code"_string };
    script.js_sources = { MUST(String::from_utf8(source)) };
    return script;
}

static Vector<String> sources(Vector<ContentScriptDescriptor> const& scripts)
{
    Vector<String> sources;
    for (auto const& script : scripts)
        sources.extend(script.js_sources);
    return sources;
}

// Stands for the page process that hosts a context; the registrations only compare it.
static int s_first_owner;
static int s_second_owner;

TEST_CASE(handles_are_unique)
{
    ExtensionContentScriptRegistrations registrations;
    auto first = registrations.add("a"_string, &s_first_owner, 1, script("1"sv));
    auto second = registrations.add("a"_string, &s_first_owner, 1, script("2"sv));
    auto third = registrations.add("b"_string, &s_first_owner, 2, script("3"sv));
    EXPECT_NE(first, second);
    EXPECT_NE(second, third);
    EXPECT_NE(first, third);
    EXPECT_EQ(sources(registrations.scripts("a"sv)), (Vector { "1"_string, "2"_string }));
    // A script's id names its registration, so the injector treats it as a registered script.
    EXPECT_EQ(registrations.scripts("a"sv)[0].id, MUST(String::formatted("contentScripts:{}", first)));
}

TEST_CASE(context_destroyed_drops_only_its_registrations)
{
    ExtensionContentScriptRegistrations registrations;
    registrations.add("a"_string, &s_first_owner, 1, script("one"sv));
    registrations.add("a"_string, &s_first_owner, 2, script("two"sv));
    registrations.add("b"_string, &s_second_owner, 1, script("other process"sv));

    EXPECT_EQ(registrations.context_destroyed(&s_first_owner, 1), (Vector { "a"_string }));
    EXPECT_EQ(sources(registrations.scripts("a"sv)), (Vector { "two"_string }));
    EXPECT_EQ(sources(registrations.scripts("b"sv)), (Vector { "other process"_string }));
    EXPECT(registrations.context_destroyed(&s_first_owner, 1).is_empty());

    EXPECT_EQ(registrations.owner_destroyed(&s_second_owner), (Vector { "b"_string }));
    EXPECT(registrations.scripts("b"sv).is_empty());
}

TEST_CASE(remove_of_an_unknown_handle_is_false)
{
    ExtensionContentScriptRegistrations registrations;
    auto handle = registrations.add("a"_string, &s_first_owner, 1, script("1"sv));
    EXPECT(!registrations.remove("b"sv, handle));
    EXPECT(registrations.remove("a"sv, handle));
    EXPECT(!registrations.remove("a"sv, handle));
    EXPECT(registrations.scripts("a"sv).is_empty());
}

TEST_CASE(registrations_go_when_the_extension_is_disabled)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(LexicalPath::join(directory.path, "Settings.json"sv).string());
    WebView::ExtensionRegistry registry { settings };
    ExtensionContentScriptRegistrations registrations;
    registry.set_content_script_registrations(registrations);
    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, R"({ "manifest_version": 2, "name": "One", "version": "1" })"sv)))->id();
    auto registered = [&] { return registry.find(id)->descriptor().content_scripts.size(); };

    registrations.add(id, &s_first_owner, 1, script("1"sv));
    registry.registered_scripts_changed(id);
    EXPECT_EQ(registered(), 1u);

    registry.set_enabled(id, false);
    registry.set_enabled(id, true);
    EXPECT_EQ(registered(), 0u);
    EXPECT(registrations.scripts(id).is_empty());

    registrations.add(id, &s_first_owner, 1, script("2"sv));
    registry.registered_scripts_changed(id);
    EXPECT_EQ(registered(), 1u);
    (void)TRY_OR_FAIL(registry.reload(id));
    EXPECT_EQ(registered(), 0u);

    registrations.add(id, &s_first_owner, 1, script("3"sv));
    registry.remove(id);
    EXPECT(registrations.scripts(id).is_empty());
}

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

// Hub admits a registered script's context by the same rules the page process injects it by, globs included, so a page
// process can't claim a context the extension never asked for.
TEST_CASE(contexts_are_admitted_by_globs_too)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(LexicalPath::join(directory.path, "Settings.json"sv).string());
    WebView::ExtensionRegistry registry { settings };
    ExtensionContentScriptRegistrations registrations;
    registry.set_content_script_registrations(registrations);
    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("globs"sv, R"({ "manifest_version": 2, "name": "Globs", "version": "1", "permissions": ["https://example.com/*"] })"sv)))->id();
    auto globbed = script("1"sv);
    globbed.matches = { MUST(Web::Extensions::MatchPattern::parse("https://example.com/*"sv)) };
    globbed.include_globs = { "*/articles/*"_string };
    globbed.exclude_globs = { "*/private/*"_string };
    registrations.add(id, &s_first_owner, 1, move(globbed));
    registry.registered_scripts_changed(id);

    EXPECT(registry.may_create_context(id, url("https://example.com/articles/1"sv), true));
    EXPECT(!registry.may_create_context(id, url("https://example.com/private/articles/1"sv), true));
    EXPECT(!registry.may_create_context(id, url("https://example.com/other"sv), true));
}

// A script that matches by its creator's origin is admitted in an about:blank, data:, or blob: document by that origin.
TEST_CASE(contexts_are_admitted_by_creator_origin)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(LexicalPath::join(directory.path, "Settings.json"sv).string());
    WebView::ExtensionRegistry registry { settings };
    ExtensionContentScriptRegistrations registrations;
    registry.set_content_script_registrations(registrations);
    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("fallback"sv, R"({ "manifest_version": 2, "name": "Fallback", "version": "1", "permissions": ["https://example.com/*"] })"sv)))->id();
    auto fallback = script("1"sv);
    fallback.matches = { MUST(Web::Extensions::MatchPattern::parse("https://example.com/"sv)) };
    fallback.all_frames = true;
    fallback.match_origin_as_fallback = true;
    registrations.add(id, &s_first_owner, 1, move(fallback));
    registry.registered_scripts_changed(id);

    auto origin = url("https://example.com/"sv);
    // An about:blank child is matched by its creator's URL, which the plain rule needs match_about_blank for.
    EXPECT(registry.may_create_context(id, url("https://example.com/page"sv), false, true, WebView::IsPrivate::No, origin));
    EXPECT(registry.may_create_context(id, url("data:text/html,hi"sv), false, false, WebView::IsPrivate::No, origin));
    EXPECT(!registry.may_create_context(id, url("data:text/html,hi"sv), false, false, WebView::IsPrivate::No, url("https://other.example/"sv)));
    EXPECT(!registry.may_create_context(id, url("data:text/html,hi"sv), false, false));
}

namespace {

struct ScriptsObserver final : WebView::ExtensionRegistryObserver {
    AK_ALLOC_WITH_KMALLOC;

    using ExtensionRegistryObserver::ExtensionRegistryObserver;
    virtual void extensions_changed() override { ++changes; }
    virtual void registered_scripts_changed(WebView::Extension const& extension) override { scripts_changed.append(extension.id()); }

    size_t changes { 0 };
    Vector<String> scripts_changed;
};

}

// A registration changes the extension's registered scripts alone, so observers hear of just that, not of a new set.
TEST_CASE(registering_scripts_tells_observers_of_the_scripts_alone)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(LexicalPath::join(directory.path, "Settings.json"sv).string());
    WebView::ExtensionRegistry registry { settings };
    ExtensionContentScriptRegistrations registrations;
    registry.set_content_script_registrations(registrations);
    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, R"({ "manifest_version": 2, "name": "One", "version": "1" })"sv)))->id();
    ScriptsObserver observer { registry };

    registrations.add(id, &s_first_owner, 1, script("1"sv));
    registry.registered_scripts_changed(id);
    EXPECT_EQ(observer.changes, 0u);
    EXPECT_EQ(observer.scripts_changed, (Vector { id }));
    EXPECT_EQ(registry.find(id)->descriptor().registered_content_scripts().size(), 1u);
}
