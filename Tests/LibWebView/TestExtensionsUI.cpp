/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>
#include <LibWebView/WebUI/ExtensionsUI.h>

namespace {

struct InstallFixture {
    InstallFixture()
        : directory(LexicalPath::join(Core::StandardPaths::tempfile_directory(), generate_random_uuid()).string())
        , settings(WebView::Settings::create(LexicalPath::join(directory, "Settings.json"sv).string()))
        , registry(settings)
    {
        MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(LexicalPath::join(directory, "manifest.json"sv).string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(R"({"manifest_version":3,"name":"Pending","version":"1","permissions":["storage"]})"sv.bytes()));
    }

    ~InstallFixture() { MUST(FileSystem::remove(directory, FileSystem::RecursionMode::Allowed)); }

    ByteString directory;
    WebView::Settings settings;
    WebView::ExtensionRegistry registry;
};

}

TEST_CASE(cancelled_install_token_is_dead)
{
    InstallFixture fixture;
    WebView::ExtensionInstallRequests requests;
    auto pending = TRY_OR_FAIL(fixture.registry.prepare_unpacked(fixture.directory));
    auto token = requests.begin(move(pending));
    requests.cancel(token);
    EXPECT(!requests.take(token).has_value());
    EXPECT(fixture.registry.extensions().is_empty());
    EXPECT(fixture.settings.installed_extensions().is_empty());
    auto persisted = WebView::Settings::create(LexicalPath::join(fixture.directory, "Settings.json"sv).string());
    EXPECT(persisted.installed_extensions().is_empty());
}

TEST_CASE(a_new_request_replaces_the_old_token)
{
    InstallFixture fixture;
    WebView::ExtensionInstallRequests requests;
    auto first = requests.begin(TRY_OR_FAIL(fixture.registry.prepare_unpacked(fixture.directory)));
    auto second = requests.begin(TRY_OR_FAIL(fixture.registry.prepare_unpacked(fixture.directory)));
    EXPECT_NE(first, second);
    EXPECT(!requests.take(first).has_value());
    requests.cancel(first);
    EXPECT(requests.take(second).has_value());
    EXPECT(!requests.take(second).has_value());
}

TEST_CASE(cleared_request_cannot_be_installed)
{
    InstallFixture fixture;
    WebView::ExtensionInstallRequests requests;
    auto token = requests.begin(TRY_OR_FAIL(fixture.registry.prepare_unpacked(fixture.directory)));
    requests.clear();
    EXPECT(!requests.take(token).has_value());
}

TEST_CASE(extensions_page_is_dynamic)
{
    auto page = WebView::WebUI::page_for_host("extensions"sv);
    EXPECT(page.has_value());
    if (page.has_value()) {
        EXPECT_EQ(page->type, WebView::WebUI::PageType::Dynamic);
        EXPECT_EQ(page->title, "Extensions"sv);
    }
}

TEST_CASE(the_last_seen_version_persists)
{
    InstallFixture fixture;
    auto id = fixture.settings.add_installed_extension(fixture.directory);
    EXPECT(!fixture.settings.installed_extensions()[0].last_seen_version.has_value());
    fixture.settings.set_installed_extension_last_seen_version(id, "1.2"_string);
    auto persisted = WebView::Settings::create(LexicalPath::join(fixture.directory, "Settings.json"sv).string());
    EXPECT_EQ(persisted.installed_extensions()[0].last_seen_version, "1.2"sv);
}
