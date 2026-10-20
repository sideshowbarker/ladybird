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
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>

using WebView::ExtensionRegistry;

namespace {

struct TemporaryDirectory {
    TemporaryDirectory()
    {
        path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-extension-registry-{}-{}", Core::System::getpid(), ++s_counter)).string();
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

    ByteString settings_path() const { return LexicalPath::join(path, "Settings.json"sv).string(); }

    ByteString path;
    static inline int s_counter = 0;
};

}

static constexpr auto minimal_manifest = R"({ "manifest_version": 3, "name": "Minimal", "version": "1" })"sv;

TEST_CASE(load_unpacked_assigns_a_uuid_and_persists)
{
    TemporaryDirectory directory;
    auto first_path = directory.extension("first"sv, minimal_manifest);
    auto second_path = directory.extension("second"sv, minimal_manifest);

    String first_id;
    {
        auto settings = WebView::Settings::create(directory.settings_path());
        ExtensionRegistry registry { settings };

        auto first = TRY_OR_FAIL(registry.load_unpacked(first_path));
        first_id = first->id();
        EXPECT_EQ(first_id.bytes_as_string_view().length(), 36u);
        EXPECT_EQ(first_id.bytes_as_string_view()[8], '-');

        auto again = TRY_OR_FAIL(registry.load_unpacked(first_path));
        EXPECT_EQ(again->id(), first_id);
        EXPECT_EQ(registry.extensions().size(), 1u);

        auto second = TRY_OR_FAIL(registry.load_unpacked(second_path));
        EXPECT_NE(second->id(), first_id);
        EXPECT_EQ(registry.extensions().size(), 2u);

        auto const& installed = settings.installed_extensions();
        EXPECT_EQ(installed.size(), 2u);
        EXPECT_EQ(installed[0].id, first_id);
        EXPECT_EQ(installed[0].path, first_path);
        EXPECT(installed[0].enabled);
    }

    auto reloaded_settings = WebView::Settings::create(directory.settings_path());
    EXPECT_EQ(reloaded_settings.installed_extensions().size(), 2u);
    EXPECT_EQ(reloaded_settings.installed_extensions()[0].id, first_id);
}

TEST_CASE(restore_from_settings_reloads_enabled_extensions_and_skips_missing_directories)
{
    TemporaryDirectory directory;
    auto present = directory.extension("present"sv, minimal_manifest);
    auto disabled = directory.extension("disabled"sv, minimal_manifest);

    String present_id;
    {
        auto settings = WebView::Settings::create(directory.settings_path());
        ExtensionRegistry registry { settings };
        present_id = TRY_OR_FAIL(registry.load_unpacked(present))->id();
        auto disabled_id = TRY_OR_FAIL(registry.load_unpacked(disabled))->id();
        registry.set_enabled(disabled_id, false);
        settings.add_installed_extension(LexicalPath::join(directory.path, "gone"sv).string());
    }

    auto settings = WebView::Settings::create(directory.settings_path());
    EXPECT_EQ(settings.installed_extensions().size(), 3u);

    ExtensionRegistry registry { settings };
    registry.restore_from_settings();

    EXPECT_EQ(registry.extensions().size(), 2u);
    EXPECT(registry.find(present_id));
    EXPECT_EQ(registry.enabled_descriptors().size(), 1u);
    EXPECT_EQ(registry.enabled_descriptors()[0].id, present_id);
    EXPECT_EQ(settings.installed_extensions().size(), 2u);
}

TEST_CASE(set_enabled_false_drops_the_descriptor)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, minimal_manifest)))->id();

    EXPECT_EQ(registry.enabled_descriptors().size(), 1u);
    registry.set_enabled(id, false);
    EXPECT_EQ(registry.enabled_descriptors().size(), 0u);
    EXPECT_EQ(registry.extensions().size(), 1u);
    EXPECT(!registry.is_enabled(id));
    EXPECT(!settings.installed_extensions()[0].enabled);

    registry.set_enabled(id, true);
    EXPECT_EQ(registry.enabled_descriptors().size(), 1u);

    registry.remove(id);
    EXPECT_EQ(registry.extensions().size(), 0u);
    EXPECT(!registry.find(id));
    EXPECT(settings.installed_extensions().is_empty());
}

TEST_CASE(on_changed_fires_on_load_remove_and_toggle)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    int changes = 0;
    registry.on_changed = [&] { ++changes; };

    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, minimal_manifest)))->id();
    EXPECT_EQ(changes, 1);
    registry.set_enabled(id, false);
    EXPECT_EQ(changes, 2);
    registry.set_enabled(id, false);
    EXPECT_EQ(changes, 2);
    registry.remove(id);
    EXPECT_EQ(changes, 3);

    EXPECT(registry.load_unpacked(LexicalPath::join(directory.path, "missing"sv).string()).is_error());
    EXPECT_EQ(changes, 3);
}

TEST_CASE(reloading_and_reenabling_changes_the_extension_instance)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto path = directory.extension("one"sv, minimal_manifest);
    auto first = TRY_OR_FAIL(registry.load_unpacked(path))->descriptor();
    auto second = TRY_OR_FAIL(registry.load_unpacked(path))->descriptor();
    EXPECT_EQ(first.id, second.id);
    EXPECT(first != second);
    registry.set_enabled(first.id, false);
    registry.set_enabled(first.id, true);
    EXPECT(second != registry.find(first.id)->descriptor());
}

TEST_CASE(relative_installation_survives_a_working_directory_change)
{
    TemporaryDirectory original_directory;
    TemporaryDirectory other_directory;
    auto path = original_directory.extension("extension"sv, minimal_manifest);
    auto original_cwd = MUST(Core::System::getcwd());
    ScopeGuard restore_cwd([&] { MUST(Core::System::chdir(original_cwd)); });
    MUST(Core::System::chdir(original_directory.path));
    String id;
    {
        auto settings = WebView::Settings::create(original_directory.settings_path());
        ExtensionRegistry registry { settings };
        auto extension = TRY_OR_FAIL(registry.load_unpacked("./extension"));
        id = extension->id();
        EXPECT_EQ(settings.installed_extensions()[0].path, MUST(FileSystem::real_path(path)));
        auto same = TRY_OR_FAIL(registry.load_unpacked(path));
        EXPECT_EQ(same->id(), id);
        EXPECT_EQ(settings.installed_extensions().size(), 1u);
    }
    MUST(Core::System::chdir(other_directory.path));
    auto settings = WebView::Settings::create(original_directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.restore_from_settings();
    EXPECT(registry.find(id));
    EXPECT_EQ(settings.installed_extensions().size(), 1u);
}

TEST_CASE(normalizing_an_installed_path_preserves_disabled_state)
{
    TemporaryDirectory directory;
    auto path = directory.extension("extension"sv, minimal_manifest);
    auto settings = WebView::Settings::create(directory.settings_path());
    auto id = settings.add_installed_extension(ByteString::formatted("{}/../extension", path));
    settings.set_installed_extension_enabled(id, false);
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(path));
    EXPECT_EQ(extension->id(), id);
    EXPECT_EQ(settings.installed_extensions().size(), 1u);
    EXPECT_EQ(settings.installed_extensions()[0].path, path);
    EXPECT(!registry.is_enabled(id));
}
