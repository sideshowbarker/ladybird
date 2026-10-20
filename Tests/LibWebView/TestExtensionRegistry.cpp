/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionBackgroundHost.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>
#include <LibWebView/ViewImplementation.h>

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

TEST_CASE(resource_requests_require_enabled_extension_and_page_authorization)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, R"({
        "manifest_version": 3, "name": "Resources", "version": "1",
        "web_accessible_resources": [{ "resources": ["public/*"], "matches": ["https://allowed.test/*"] }]
    })"sv)));
    auto const& id = extension->id();
    auto allowed = URL::Parser::basic_parse("https://allowed.test/page"sv).release_value();
    auto unrelated = URL::Parser::basic_parse("https://unrelated.test/page"sv).release_value();
    auto own = URL::Parser::basic_parse(MUST(String::formatted("ladybird-extension://{}/page.html", id))).release_value();
    EXPECT(registry.may_access_resource(id, "/public/file.txt"sv, allowed));
    EXPECT(!registry.may_access_resource(id, "/private.txt"sv, allowed));
    EXPECT(!registry.may_access_resource(id, "/public/file.txt"sv, unrelated));
    EXPECT(registry.may_access_resource(id, "/private.txt"sv, own));
    registry.set_enabled(id, false);
    EXPECT(!registry.may_access_resource(id, "/private.txt"sv, own));
    EXPECT(!registry.may_access_resource(id, "/public/file.txt"sv, allowed));
}

TEST_CASE(context_registration_requires_a_matching_document)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, R"({
        "manifest_version": 3, "name": "Contexts", "version": "1",
        "content_scripts": [{ "matches": ["https://allowed.test/*"], "exclude_matches": ["https://allowed.test/private/*"] }]
    })"sv)));
    auto const& id = extension->id();
    auto url = [](StringView value) { return URL::Parser::basic_parse(value).release_value(); };
    EXPECT(registry.may_create_context(id, url("https://allowed.test/page"sv), true));
    EXPECT(!registry.may_create_context(id, url("https://allowed.test/private/page"sv), true));
    EXPECT(!registry.may_create_context(id, url("https://unrelated.test/page"sv), true));
    EXPECT(!registry.may_create_context(id, url("https://allowed.test/page"sv), false));
    auto own = url(MUST(String::formatted("ladybird-extension://{}/page.html", id)));
    EXPECT(registry.may_create_context(id, own, false));
    EXPECT(!registry.may_create_context(id, own, false, true));
    EXPECT(!registry.may_create_context(id, url("https://allowed.test/page"sv), true, true));
    registry.set_enabled(id, false);
    EXPECT(!registry.may_create_context(id, own, true));
}

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

class ExtensionTestApplication final : public WebView::Application {
    WEB_VIEW_APPLICATION(ExtensionTestApplication)

public:
    explicit ExtensionTestApplication(Optional<ByteString> binary_path)
        : WebView::Application(move(binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser, WebView::RequestServerOptions&, WebView::WebContentOptions& content) override
    {
        browser.headless_mode.clear();
        browser.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        content.is_test_mode = WebView::IsTestMode::Yes;
    }
    virtual Core::EventLoop& create_platform_event_loop() override { return Core::EventLoop::current(); }
    virtual bool should_coordinate_browser_process() const override { return false; }
};

class StartupMessageEndpoint final : public WebView::ExtensionMessageBroker::Endpoint {
public:
    u64 revision { 0 };
    size_t completions { 0 };
    virtual Optional<u64> extension_revision(String const&) const override { return revision; }
    virtual Optional<Web::Extensions::MessageSender> extension_message_sender(String const& id, Web::HTML::CrossProcessId) const override
    {
        return Web::Extensions::MessageSender { .extension_id = id, .url = "https://example.com/"_string, .from_background = false };
    }
    virtual void dispatch_extension_runtime_message(String const&, u64, Web::Extensions::MessageSender const&, String const&, u64) override { VERIFY_NOT_REACHED(); }
    virtual void complete_extension_runtime_message(u64, Optional<String> const& json) override
    {
        EXPECT(!json.has_value());
        ++completions;
    }
};

TEST_CASE(reloading_replaces_the_background_view)
{
    Core::EventLoop event_loop;
    TemporaryDirectory directory;
    ScopeGuard cleanup_queue([] { Core::ThreadEventQueue::destroy_current(); });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, directory.path, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, directory.path, Core::Environment::Overwrite::Yes));
    StringView executable = "ladybird"sv;
    Main::Arguments arguments { .argc = 0, .argv = nullptr, .strings = Span<StringView> { &executable, 1 } };
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY_OR_FAIL(ExtensionTestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY_OR_FAIL(ExtensionTestApplication::create(arguments, OptionalNone {}));
#endif
    auto& registry = WebView::Application::extension_registry();
    WebView::ExtensionBackgroundHost host;
    auto path = directory.extension("background"sv, R"({
        "manifest_version": 2, "name": "Background", "version": "1",
        "background": { "scripts": [] }
    })"sv);
    auto extension = TRY_OR_FAIL(registry.load_unpacked(path));
    auto& broker = WebView::Application::extension_message_broker();
    StartupMessageEndpoint sender;
    sender.revision = extension->descriptor().revision;
    broker.context_created(sender, extension->id(), 1, { 1, 1 }, sender.revision);
    ScopeGuard remove_sender([&] { broker.endpoint_gone(sender); });
    broker.send(sender, extension->id(), 1, {}, "1"_string, 1);
    EXPECT_EQ(sender.completions, 0u);
    auto background_id = [&]() -> Optional<Web::HTML::CrossProcessId> {
        Optional<Web::HTML::CrossProcessId> result;
        WebView::ViewImplementation::for_each_view([&](auto& view) {
            if (!host.hosts_page(extension->id(), view.page()))
                return IterationDecision::Continue;
            result = view.traversable().id();
            return IterationDecision::Break;
        });
        return result;
    };
    host.sync(registry);
    auto first = background_id();
    EXPECT(first.has_value());
    auto replacement = TRY_OR_FAIL(registry.load_unpacked(path));
    EXPECT_EQ(sender.completions, 1u);
    sender.revision = replacement->descriptor().revision;
    broker.context_created(sender, extension->id(), 2, { 1, 1 }, sender.revision);
    broker.send(sender, extension->id(), 2, {}, "2"_string, 2);
    EXPECT_EQ(sender.completions, 1u);
    host.sync(registry);
    auto second = background_id();
    EXPECT(second.has_value());
    EXPECT(first != second);
    registry.set_enabled(extension->id(), false);
    EXPECT_EQ(sender.completions, 2u);
    host.sync(registry);
    EXPECT(!background_id().has_value());
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
