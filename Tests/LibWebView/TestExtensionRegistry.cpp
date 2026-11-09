/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ExtensionArchiveTestHelpers.h"
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
#include <LibWebView/Extensions/ExtensionArchive.h>
#include <LibWebView/Extensions/ExtensionBackgroundHost.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionPermissionsAPI.h>
#include <LibWebView/Extensions/ExtensionRegisteredScripts.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Extensions/ExtensionScriptingAPI.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>
#include <LibWebView/Settings.h>
#include <LibWebView/ViewImplementation.h>
#include <unistd.h>

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

struct TestRegistryObserver final : WebView::ExtensionRegistryObserver {
    AK_ALLOC_WITH_KMALLOC;

    TestRegistryObserver(ExtensionRegistry& registry, Function<void()> callback)
        : ExtensionRegistryObserver(registry)
        , callback(move(callback))
    {
    }
    virtual void extensions_changed() override { callback(); }
    Function<void()> callback;
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

TEST_CASE(restore_keeps_failed_entries)
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
    EXPECT(registry.enabled_descriptors(WebView::IsPrivate::Yes).is_empty());
    EXPECT_EQ(settings.installed_extensions().size(), 3u);
    EXPECT_EQ(registry.failed_extensions().size(), 1u);
    EXPECT(!registry.failed_extensions()[0].error.is_empty());
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

TEST_CASE(observers_hear_every_change)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    int changes = 0;
    TestRegistryObserver observer(registry, [&] { ++changes; });

    auto id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, minimal_manifest)))->id();
    EXPECT_EQ(changes, 1);
    registry.set_enabled(id, false);
    EXPECT_EQ(changes, 2);
    registry.set_enabled(id, false);
    EXPECT_EQ(changes, 2);
    registry.set_allowed_in_private(id, true);
    EXPECT_EQ(changes, 3);
    (void)TRY_OR_FAIL(registry.reload(id));
    EXPECT_EQ(changes, 4);
    registry.remove(id);
    EXPECT_EQ(changes, 5);

    EXPECT(registry.load_unpacked(LexicalPath::join(directory.path, "missing"sv).string()).is_error());
    EXPECT_EQ(changes, 5);
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
    virtual void dispatch_extension_runtime_connect(u64, u64, u64, String const&, Web::Extensions::MessageSender const&) override { }
    virtual void complete_extension_runtime_connect(u64, u64, u64, Optional<String> const&) override { }
    virtual void dispatch_extension_port_message(u64, u64, u64, String const&) override { }
    virtual void dispatch_extension_port_disconnect(u64, u64, Optional<String> const&) override { }

    u64 revision { 0 };
    size_t completions { 0 };
    virtual Optional<u64> extension_revision(String const&) const override { return revision; }
    virtual Optional<Web::Extensions::MessageSender> extension_message_sender(String const& id, Web::HTML::CrossProcessId) const override
    {
        return Web::Extensions::MessageSender { .extension_id = id, .url = "https://example.com/"_string, .from_background = false };
    }
    virtual void dispatch_extension_runtime_message(String const&, u64, Web::Extensions::MessageSender const&, String const&, u64) override { VERIFY_NOT_REACHED(); }
    virtual void complete_extension_runtime_message(u64, Web::Extensions::MessageResult const& result) override
    {
        EXPECT(result.value.has<Web::Extensions::MessageResult::Error>());
        EXPECT_EQ(result.value.get<Web::Extensions::MessageResult::Error>().message, "Could not establish connection. Receiving end does not exist."sv);
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

TEST_CASE(command_line_reload_keeps_the_grant)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto path = directory.extension("permissions"sv, R"({
        "manifest_version": 2, "name": "Permissions", "version": "1",
        "permissions": ["storage", "storage", "https://example.com/*"]
    })"sv);
    auto extension = TRY_OR_FAIL(registry.load_unpacked(path));
    auto id = extension->id();
    EXPECT_EQ(extension->descriptor().api_permissions, (Vector { "storage"_string }));
    EXPECT(extension->descriptor().has_permission("storage"sv));
    EXPECT_EQ(extension->descriptor().host_permissions.size(), 1u);
    EXPECT(extension->descriptor().host_permissions[0].matches(URL::Parser::basic_parse("https://example.com/a"sv).release_value()));
    EXPECT_EQ(settings.installed_extensions()[0].granted_permissions, (Vector { "storage"_string }));

    directory.extension("permissions"sv, R"({
        "manifest_version": 3, "name": "Permissions", "version": "2",
        "permissions": ["tabs", "storage", "unlimitedStorage"],
        "host_permissions": ["https://other.test/*"]
    })"sv);
    extension = TRY_OR_FAIL(registry.load_unpacked(path));
    EXPECT_EQ(extension->id(), id);
    EXPECT_EQ(extension->descriptor().api_permissions, (Vector { "storage"_string }));
    EXPECT(!extension->descriptor().has_permission("tabs"sv));
    EXPECT(!extension->descriptor().has_permission("unlimitedStorage"sv));
    EXPECT_EQ(extension->descriptor().host_permissions.size(), 1u);
    EXPECT(extension->descriptor().host_permissions[0].matches(URL::Parser::basic_parse("https://other.test/a"sv).release_value()));

    auto restored_settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry restored { restored_settings };
    restored.restore_from_settings();
    EXPECT_EQ(restored.find(id)->descriptor().api_permissions, (Vector { "storage"_string }));
    EXPECT_EQ(restored_settings.installed_extensions()[0].granted_permissions, (Vector { "storage"_string }));
}

TEST_CASE(first_install_grants_sorted_supported_permissions)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("permissions"sv, R"({
        "manifest_version": 3, "name": "Permissions", "version": "1",
        "permissions": ["unlimitedStorage", "tabs", "storage", "unknown"]
    })"sv)));
    EXPECT_EQ(extension->descriptor().api_permissions, (Vector { "storage"_string, "tabs"_string, "unlimitedStorage"_string }));
}

TEST_CASE(private_access_defaults_to_false_in_settings)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("private"sv, minimal_manifest)));
    EXPECT(!extension->id().is_empty());
    auto file = TRY_OR_FAIL(Core::File::open(directory.settings_path(), Core::File::OpenMode::Read));
    auto bytes = TRY_OR_FAIL(file->read_until_eof());
    auto json = TRY_OR_FAIL(JsonValue::from_string(bytes));
    auto const& entry = json.as_object().get_array("extensions"sv)->at(0).as_object();
    EXPECT(entry.get_bool("allowed_in_private"sv).has_value());
    EXPECT_EQ(entry.get_bool("allowed_in_private"sv).value_or(true), false);
}

TEST_CASE(private_opt_in_filters_descriptors_and_notifies)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("private"sv, minimal_manifest)));
    auto id = extension->id();
    auto own = URL::Parser::basic_parse(MUST(String::formatted("ladybird-extension://{}/page.html", id))).release_value();
    EXPECT(!registry.is_allowed_in_private(id));
    EXPECT_EQ(registry.enabled_descriptors(WebView::IsPrivate::No).size(), 1u);
    EXPECT(registry.enabled_descriptors(WebView::IsPrivate::Yes).is_empty());
    EXPECT(!registry.may_create_context(id, own, true, false, WebView::IsPrivate::Yes));
    EXPECT(!registry.may_access_resource(id, "/manifest.json"sv, own, WebView::IsPrivate::Yes));
    size_t notifications = 0;
    TestRegistryObserver observer(registry, [&] { ++notifications; });
    registry.set_allowed_in_private(id, true);
    EXPECT_EQ(notifications, 1u);
    EXPECT_EQ(registry.enabled_descriptors(WebView::IsPrivate::Yes).size(), 1u);
    EXPECT(registry.may_create_context(id, own, true, false, WebView::IsPrivate::Yes));
    EXPECT(registry.may_access_resource(id, "/manifest.json"sv, own, WebView::IsPrivate::Yes));
    auto persisted = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry restored { persisted };
    restored.restore_from_settings();
    EXPECT(restored.is_allowed_in_private(id));
    registry.set_allowed_in_private(id, true);
    EXPECT_EQ(notifications, 1u);
    registry.set_allowed_in_private(id, false);
    EXPECT_EQ(notifications, 2u);
    EXPECT(registry.enabled_descriptors(WebView::IsPrivate::Yes).is_empty());
    EXPECT_EQ(registry.enabled_descriptors(WebView::IsPrivate::No).size(), 1u);
    auto disabled_settings = WebView::Settings::create(directory.settings_path());
    EXPECT(!disabled_settings.installed_extensions()[0].allowed_in_private);
}

TEST_CASE(disable_keeps_storage_and_remove_deletes_after_revocation)
{
    Core::EventLoop loop;
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    WebView::ExtensionStorageStore store(directory.path);
    ExtensionRegistry registry(settings);
    registry.set_storage_store(store);
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, minimal_manifest)));
    auto id = extension->id();
    auto file = LexicalPath::join(directory.path, "extensions"sv, id, "storage-local.json"sv).string();
    bool written = false;
    JsonObject items;
    items.set("key"sv, 1);
    store.set(id, items, false, [&](auto result) { EXPECT(!result.is_error()); written = true; });
    loop.spin_until([&] { return written; });
    registry.set_enabled(id, false);
    EXPECT(FileSystem::exists(file));
    bool read = false;
    store.get(id, {}, [&](auto result) { EXPECT(!result.is_error()); if (!result.is_error()) EXPECT_EQ(result.value().get_i32("key"sv).value(), 1); read = true; });
    loop.spin_until([&] { return read; });
    registry.set_enabled(id, true);
    bool revoked = false;
    TestRegistryObserver observer(registry, [&] { revoked = !registry.find(id) && !registry.is_enabled(id); });
    items.set("key"sv, 2);
    store.set(id, move(items), false, [](auto result) { EXPECT(!result.is_error()); });
    registry.remove(id);
    EXPECT(revoked);
    read = false;
    store.get(id, {}, [&](auto result) { EXPECT(!result.is_error()); if (!result.is_error()) EXPECT(result.value().is_empty()); read = true; });
    loop.spin_until([&] { return read; });
    EXPECT(!FileSystem::exists(LexicalPath(file).dirname()));
}

TEST_CASE(prepared_install_persists_nothing)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry(settings);
    auto path = directory.extension("prepared"sv, minimal_manifest);
    auto before = settings.serialize_json().serialized();
    auto pending = TRY_OR_FAIL(registry.prepare_unpacked(path));
    EXPECT(!pending.is_reload);
    EXPECT(registry.extensions().is_empty());
    EXPECT_EQ(settings.serialize_json().serialized(), before);
    auto persisted = WebView::Settings::create(directory.settings_path());
    EXPECT(persisted.installed_extensions().is_empty());
}

TEST_CASE(first_install_grants_the_declared_permissions)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry(settings);
    auto pending = TRY_OR_FAIL(registry.prepare_unpacked(directory.extension("permissions"sv, R"({
        "manifest_version": 3, "name": "Permissions", "version": "1",
        "permissions": ["tabs", "storage", "unknown", "storage"]
    })"sv)));
    EXPECT_EQ(pending.newly_declared_permissions, (Vector { "storage"_string, "tabs"_string }));
    auto extension = registry.install(move(pending));
    EXPECT_EQ(extension->descriptor().api_permissions, (Vector { "storage"_string, "tabs"_string }));
    auto persisted = WebView::Settings::create(directory.settings_path());
    EXPECT_EQ(persisted.installed_extensions()[0].granted_permissions, (Vector { "storage"_string, "tabs"_string }));
}

TEST_CASE(prepare_of_installed_directory_is_a_reload)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry(settings);
    auto path = directory.extension("permissions"sv, R"({
        "manifest_version": 3, "name": "Permissions", "version": "1", "permissions": ["storage"]
    })"sv);
    auto original = TRY_OR_FAIL(registry.load_unpacked(path));
    registry.set_enabled(original->id(), false);
    registry.set_allowed_in_private(original->id(), true);
    directory.extension("permissions"sv, R"({
        "manifest_version": 3, "name": "Permissions", "version": "2", "permissions": ["tabs"]
    })"sv);
    auto pending = TRY_OR_FAIL(registry.prepare_unpacked(ByteString::formatted("{}/../permissions", path)));
    EXPECT(pending.is_reload);
    EXPECT_EQ(pending.extension->id(), original->id());
    EXPECT_EQ(pending.newly_declared_permissions, (Vector { "tabs"_string }));
    EXPECT_EQ(registry.find(original->id())->descriptor().version, "1"sv);
    auto installed = registry.install(move(pending));
    EXPECT_EQ(installed->descriptor().api_permissions, (Vector { "tabs"_string }));
    EXPECT(!registry.is_enabled(installed->id()));
    EXPECT(registry.is_allowed_in_private(installed->id()));
    auto persisted = WebView::Settings::create(directory.settings_path());
    EXPECT_EQ(persisted.installed_extensions()[0].granted_permissions, (Vector { "storage"_string, "tabs"_string }));
}

TEST_CASE(failed_reload_unloads_and_records_the_error)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry(settings);
    auto path = directory.extension("broken"sv, minimal_manifest);
    auto original = TRY_OR_FAIL(registry.load_unpacked(path));
    directory.extension("broken"sv, "{}"sv);
    auto result = registry.reload(original->id());
    EXPECT(result.is_error());
    EXPECT(!registry.find(original->id()));
    EXPECT(registry.enabled_descriptors().is_empty());
    EXPECT_EQ(settings.installed_extensions().size(), 1u);
    EXPECT_EQ(registry.failed_extensions().size(), 1u);
    EXPECT_EQ(registry.failed_extensions()[0].id, original->id());
    EXPECT_EQ(registry.failed_extensions()[0].directory, path);
    EXPECT_EQ(registry.failed_extensions()[0].error, result.error());
    auto own = URL::Parser::basic_parse(MUST(String::formatted("ladybird-extension://{}/page.html", original->id()))).release_value();
    EXPECT(!registry.may_create_context(original->id(), own, true));
    EXPECT(!registry.may_access_resource(original->id(), "/manifest.json"sv, own));
}

TEST_CASE(reload_of_a_fixed_entry_loads_it)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    auto path = LexicalPath::join(directory.path, "repaired"sv).string();
    auto id = settings.add_installed_extension(path);
    ExtensionRegistry registry(settings);
    registry.restore_from_settings();
    EXPECT_EQ(registry.failed_extensions().size(), 1u);
    directory.extension("repaired"sv, minimal_manifest);
    auto extension = TRY_OR_FAIL(registry.reload(id));
    EXPECT_EQ(extension->id(), id);
    EXPECT_EQ(registry.enabled_descriptors().size(), 1u);
    EXPECT(registry.failed_extensions().is_empty());
    EXPECT_EQ(settings.installed_extensions().size(), 1u);
}

TEST_CASE(remove_of_a_failed_entry_deletes_its_storage)
{
    Core::EventLoop loop;
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    auto id = settings.add_installed_extension(LexicalPath::join(directory.path, "missing"sv).string());
    WebView::ExtensionStorageStore store(directory.path);
    ExtensionRegistry registry(settings);
    registry.set_storage_store(store);
    registry.restore_from_settings();
    JsonObject items;
    items.set("key"sv, 1);
    bool finished = false;
    store.set(id, items, false, [&](auto result) { EXPECT(!result.is_error()); finished = true; });
    loop.spin_until([&] { return finished; });
    auto storage = LexicalPath::join(directory.path, "extensions"sv, id, "storage-local.json"sv).string();
    EXPECT(FileSystem::exists(storage));
    registry.remove(id);
    EXPECT(settings.installed_extensions().is_empty());
    EXPECT(registry.failed_extensions().is_empty());
    finished = false;
    store.get(id, {}, [&](auto result) { EXPECT(!result.is_error()); if (!result.is_error()) EXPECT(result.value().is_empty()); finished = true; });
    loop.spin_until([&] { return finished; });
    EXPECT(!FileSystem::exists(storage));
}

TEST_CASE(removed_observer_hears_nothing)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry(settings);
    int changes = 0;
    {
        TestRegistryObserver observer(registry, [&] { ++changes; });
        registry.restore_from_settings();
        EXPECT_EQ(changes, 1);
    }
    (void)TRY_OR_FAIL(registry.load_unpacked(directory.extension("one"sv, minimal_manifest)));
    EXPECT_EQ(changes, 1);
}

TEST_CASE(observer_can_outlive_registry)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    OwnPtr<TestRegistryObserver> observer;
    {
        ExtensionRegistry registry(settings);
        observer = make<TestRegistryObserver>(registry, [] { });
    }
    observer.clear();
}

TEST_CASE(optional_permissions_start_ungranted_and_their_grants_persist)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto path = directory.extension("optional"sv, R"({
        "manifest_version": 3, "name": "Optional", "version": "1",
        "permissions": ["storage"], "optional_permissions": ["tabs"],
        "host_permissions": ["https://required.example/*"], "optional_host_permissions": ["https://optional.example/*"]
    })"sv);
    auto extension = TRY_OR_FAIL(registry.load_unpacked(path));
    auto id = extension->id();
    auto required = URL::Parser::basic_parse("https://required.example/a"sv).release_value();
    auto optional = URL::Parser::basic_parse("https://optional.example/a"sv).release_value();
    EXPECT_EQ(extension->descriptor().api_permissions, (Vector { "storage"_string }));
    EXPECT_EQ(extension->descriptor().optional_api_permissions, (Vector { "tabs"_string }));
    EXPECT(extension->descriptor().has_host_access(required));
    EXPECT(!extension->descriptor().has_host_access(optional));

    registry.set_granted(id, { "storage"_string, "tabs"_string }, { "https://optional.example/*"_string });
    auto granted = registry.find(id);
    EXPECT(granted->descriptor().has_permission("tabs"sv));
    EXPECT(granted->descriptor().has_host_access(optional));
    EXPECT(settings.installed_extensions()[0].granted_permissions.contains_slow("https://optional.example/*"sv));

    // A grant of something the manifest doesn't declare as optional grants nothing.
    registry.set_granted(id, { "storage"_string, "tabs"_string, "cookies"_string }, { "https://optional.example/*"_string, "https://other.example/*"_string });
    EXPECT(!registry.find(id)->descriptor().has_permission("cookies"sv));
    EXPECT(!registry.find(id)->descriptor().has_host_access(URL::Parser::basic_parse("https://other.example/a"sv).release_value()));

    // Loading it again keeps the grants.
    auto reloaded = TRY_OR_FAIL(registry.load_unpacked(path));
    EXPECT(reloaded->descriptor().has_permission("tabs"sv));
    EXPECT(reloaded->descriptor().has_host_access(optional));
}

TEST_CASE(registered_scripts_reach_the_descriptor_and_survive_a_restart)
{
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    auto path = directory.extension("registering"sv, R"({ "manifest_version": 3, "name": "Registering", "version": "1", "permissions": ["scripting"] })"sv);
    auto file = MUST(Core::File::open(LexicalPath::join(path, "cs.js"sv).string(), Core::File::OpenMode::Write));
    MUST(file->write_until_depleted("window.ran = true;"sv.bytes()));
    auto script = [](StringView id, bool persist) {
        return MUST(JsonValue::from_string(MUST(String::formatted(R"({{ "id": "{}", "matches": ["https://example.com/*"], "js": ["cs.js"], "persistAcrossSessions": {} }})", id, persist)))).as_object();
    };
    auto registered_ids = [](WebView::Extension const& extension) {
        Vector<String> ids;
        for (auto const& script : extension.descriptor().content_scripts) {
            if (script.id.has_value())
                ids.append(*script.id);
        }
        return ids;
    };

    String id;
    {
        auto settings = WebView::Settings::create(directory.settings_path());
        WebView::ExtensionRegisteredScripts scripts { profile };
        ExtensionRegistry registry { settings };
        registry.set_registered_scripts(scripts);
        id = TRY_OR_FAIL(registry.load_unpacked(path))->id();
        MUST(scripts.register_scripts(id, { script("kept"sv, true), script("session"sv, false) }));
        registry.registered_scripts_changed(id);
        auto extension = registry.find(id);
        EXPECT_EQ(registered_ids(*extension), (Vector { "kept"_string, "session"_string }));
        EXPECT_EQ(extension->descriptor().content_scripts[0].js_sources, (Vector { "window.ran = true;"_string }));
    }

    auto settings = WebView::Settings::create(directory.settings_path());
    WebView::ExtensionRegisteredScripts scripts { profile };
    ExtensionRegistry registry { settings };
    registry.set_registered_scripts(scripts);
    registry.restore_from_settings();
    EXPECT_EQ(registered_ids(*registry.find(id)), (Vector { "kept"_string }));

    // Removing the extension forgets its scripts for good.
    registry.remove(id);
    WebView::ExtensionRegisteredScripts after_removal { profile };
    EXPECT(after_removal.scripts(id).is_empty());
}

TEST_CASE(injection_refusal_keeps_browser_pages_off_limits)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("everywhere"sv, R"({ "manifest_version": 3, "name": "Everywhere", "version": "1", "permissions": ["scripting"], "host_permissions": ["<all_urls>"] })"sv)));
    auto url = [](StringView text) { return URL::Parser::basic_parse(text).release_value(); };
    EXPECT(!WebView::extension_injection_refusal(*extension, url("https://example.com/"sv), true, false).has_value());
    // A browser page stays off limits with a host permission for everything and with activeTab both.
    EXPECT_EQ(WebView::extension_injection_refusal(*extension, url("about:settings"sv), true, true), "Cannot access contents of url \"about:settings\". Extension manifest must request permission to access this host."_string);
    EXPECT(WebView::extension_injection_refusal(*extension, url("about:newtab"sv), false, true).has_value());
}

TEST_CASE(removing_an_origin_a_required_one_overlaps_changes_nothing)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    auto extension = TRY_OR_FAIL(registry.load_unpacked(directory.extension("overlapping"sv, R"({
        "manifest_version": 3, "name": "Overlapping", "version": "1",
        "optional_permissions": ["tabs"],
        "host_permissions": ["https://*.example.com/*"],
        "optional_host_permissions": ["https://a.example.com/*", "https://scripted.example.org/*", "https://free.example.net/*"],
        "content_scripts": [{ "matches": ["https://scripted.example.org/*"], "js": [] }]
    })"sv)));
    registry.set_granted(extension->id(), { "tabs"_string }, { "https://a.example.com/*"_string, "https://scripted.example.org/*"_string, "https://free.example.net/*"_string });
    extension = *registry.find(extension->id());
    auto remove = [&](Vector<String> permissions, Vector<String> origins) {
        return WebView::extension_permissions_to_remove(*extension, { move(permissions), move(origins) });
    };
    auto refusal = [&](Vector<String> permissions, Vector<String> origins) -> String {
        auto result = remove(move(permissions), move(origins));
        return result.is_error() ? result.release_error() : "removed"_string;
    };
    // A required host pattern covers the optional one, so removing it (and anything with it) is refused.
    EXPECT_EQ(refusal({ "tabs"_string }, { "https://a.example.com/*"_string }), "You cannot remove required permissions."sv);
    // So does a manifest content script's match.
    EXPECT_EQ(refusal({}, { "https://scripted.example.org/*"_string }), "You cannot remove required permissions."sv);
    auto removed = TRY_OR_FAIL(remove({ "tabs"_string }, { "https://free.example.net/*"_string }));
    EXPECT_EQ(removed.permissions, (Vector { "tabs"_string }));
    EXPECT_EQ(removed.origins, (Vector { "https://free.example.net/*"_string }));
}

static ByteString write_xpi(TemporaryDirectory& directory, StringView name, StringView manifest_json, StringView script)
{
    auto archive = make_archive({ { "manifest.json", manifest_json }, { "bg.js", script } });
    auto path = LexicalPath::join(directory.path, name).string();
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(archive));
    return path;
}

// The entries of a directory, or none if it doesn't exist.
static size_t count_entries(ByteString const& path)
{
    size_t entries = 0;
    if (!FileSystem::exists(path))
        return 0;
    MUST(Core::Directory::for_each_entry(path, Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) {
        ++entries;
        return IterationDecision::Continue;
    }));
    return entries;
}

static String read_text(ByteString const& path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(String::from_utf8(MUST(file->read_until_eof())));
}

static constexpr auto gecko_manifest = R"({ "manifest_version": 2, "name": "Packed", "version": "1", "permissions": ["storage"], "background": { "scripts": ["bg.js"] }, "browser_specific_settings": { "gecko": { "id": "packed@example.org" } } })"sv;

TEST_CASE(packed_install_extracts_into_the_profile)
{
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(profile);
    auto xpi = write_xpi(directory, "packed.xpi"sv, gecko_manifest, "// one"sv);
    auto extension = registry.install(TRY_OR_FAIL(registry.prepare_packed(xpi)));
    auto package = LexicalPath::join(profile, "extensions"sv, extension->id(), "package"sv).string();
    EXPECT_EQ(extension->directory(), package);
    EXPECT_EQ(read_text(LexicalPath::join(package, "bg.js"sv).string()), "// one"sv);
    EXPECT(settings.installed_extensions()[0].packed);
    EXPECT_EQ(settings.installed_extensions()[0].gecko_id, "packed@example.org"sv);
    // Nothing stays staged once it's installed.
    EXPECT_EQ(count_entries(LexicalPath::join(profile, "extensions"sv).string()), 1u);
}

TEST_CASE(reinstall_by_gecko_id_keeps_id_and_data)
{
    Core::EventLoop loop;
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    auto settings = WebView::Settings::create(directory.settings_path());
    WebView::ExtensionRegisteredScripts scripts { profile };
    WebView::ExtensionStorageStore store(profile);
    ExtensionRegistry registry { settings };
    registry.set_storage_store(store);
    registry.set_profile_data_directory(profile);
    registry.set_registered_scripts(scripts);
    auto first = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "v1.xpi"sv, gecko_manifest, "// one"sv))));
    auto id = first->id();
    registry.set_enabled(id, false);
    registry.set_enabled(id, true);
    MUST(scripts.register_scripts(id, { MUST(JsonValue::from_string(R"({ "id": "kept", "matches": ["https://example.com/*"], "js": ["bg.js"] })"sv)).as_object() }));
    JsonObject items;
    items.set("secret"sv, 42);
    bool written = false;
    store.set(id, items, false, [&](auto result) { EXPECT(!result.is_error()); written = true; });
    loop.spin_until([&] { return written; });

    auto pending = TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "v2.xpi"sv, gecko_manifest, "// two"sv)));
    EXPECT(pending.is_reload);
    auto second = registry.install(move(pending));
    EXPECT_EQ(second->id(), id);
    EXPECT_EQ(settings.installed_extensions().size(), 1u);
    EXPECT_EQ(read_text(LexicalPath::join(second->directory(), "bg.js"sv).string()), "// two"sv);
    EXPECT(second->descriptor().has_permission("storage"sv));
    EXPECT(second->descriptor().content_scripts.contains([](auto const& script) { return script.id == "kept"sv; }));
    bool read = false;
    store.get(id, {}, [&](auto result) {
        EXPECT(!result.is_error() && result.value().get_i32("secret"sv) == 42);
        read = true;
    });
    loop.spin_until([&] { return read; });
}

TEST_CASE(packed_installs_without_a_gecko_id_are_new_each_time)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    auto manifest = R"({ "manifest_version": 3, "name": "Anonymous", "version": "1" })"sv;
    auto first = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "a.xpi"sv, manifest, ""sv))));
    auto second = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "b.xpi"sv, manifest, ""sv))));
    EXPECT_NE(first->id(), second->id());
    EXPECT_EQ(settings.installed_extensions().size(), 2u);
}

TEST_CASE(removing_deletes_a_package_but_not_an_unpacked_folder)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    auto packed = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "p.xpi"sv, gecko_manifest, ""sv))));
    auto package = packed->directory();
    auto folder = directory.extension("unpacked"sv, minimal_manifest);
    auto unpacked = TRY_OR_FAIL(registry.load_unpacked(folder));
    registry.remove(packed->id());
    registry.remove(unpacked->id());
    EXPECT(!FileSystem::exists(package));
    EXPECT(FileSystem::exists(folder));
}

TEST_CASE(a_bad_archive_installs_nothing)
{
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(profile);
    auto path = LexicalPath::join(directory.path, "bad.xpi"sv).string();
    MUST(MUST(Core::File::open(path, Core::File::OpenMode::Write))->write_until_depleted("not a zip"sv.bytes()));
    auto result = registry.prepare_packed(path);
    EXPECT(result.is_error());
    // A good archive whose manifest is broken leaves nothing staged either.
    auto broken = registry.prepare_packed(write_xpi(directory, "broken.xpi"sv, "{"sv, ""sv));
    EXPECT(broken.is_error());
    EXPECT(settings.installed_extensions().is_empty());
    EXPECT_EQ(count_entries(LexicalPath::join(profile, "extensions"sv).string()), 0u);
}

TEST_CASE(hidden_in_toolbar_persists_and_survives_a_reload)
{
    TemporaryDirectory directory;
    String id;
    {
        auto settings = WebView::Settings::create(directory.settings_path());
        ExtensionRegistry registry { settings };
        id = TRY_OR_FAIL(registry.load_unpacked(directory.extension("button"sv, R"({ "manifest_version": 3, "name": "Button", "version": "1", "action": {} })"sv)))->id();
        EXPECT(!registry.is_hidden_in_toolbar(id));
        registry.set_hidden_in_toolbar(id, true);
        EXPECT(registry.is_hidden_in_toolbar(id));
        (void)TRY_OR_FAIL(registry.reload(id));
        EXPECT(registry.is_hidden_in_toolbar(id));
    }
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.restore_from_settings();
    EXPECT(registry.is_hidden_in_toolbar(id));
    registry.set_hidden_in_toolbar(id, false);
    EXPECT(!WebView::Settings::create(directory.settings_path()).installed_extensions()[0].hidden_in_toolbar);
}

TEST_CASE(an_oversized_archive_is_refused_before_it_is_read)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    // A sparse file, so the test writes nothing near its size.
    auto path = LexicalPath::join(directory.path, "huge.xpi"sv).string();
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    VERIFY(::ftruncate(file->fd(), static_cast<off_t>(WebView::extension_archive_max_file_size + 1)) == 0);
    auto result = registry.prepare_packed(path);
    EXPECT(result.is_error() && result.error().contains("too large"sv));
}

TEST_CASE(removing_a_package_never_follows_a_symlink_out_of_it)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    auto outside = LexicalPath::join(directory.path, "outside"sv).string();
    MUST(Core::Directory::create(outside, Core::Directory::CreateDirectories::Yes));
    auto sentinel = LexicalPath::join(outside, "keep.txt"sv).string();
    MUST(MUST(Core::File::open(sentinel, Core::File::OpenMode::Write))->write_until_depleted("keep"sv.bytes()));

    auto packed = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "p.xpi"sv, gecko_manifest, ""sv))));
    MUST(Core::System::symlink(outside, LexicalPath::join(packed->directory(), "link"sv).string()));
    auto package = packed->directory();
    registry.remove(packed->id());
    EXPECT(!FileSystem::exists(package));
    EXPECT(FileSystem::exists(sentinel));

    // A packed record whose path was pointed elsewhere doesn't delete that place.
    auto other = registry.install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "q.xpi"sv, gecko_manifest, ""sv))));
    settings.add_installed_extension(outside, other->id(), {});
    registry.remove(other->id());
    EXPECT(FileSystem::exists(sentinel));
}

TEST_CASE(replacing_a_packed_extension_always_asks)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    auto quiet = R"({ "manifest_version": 3, "name": "Quiet", "version": "1", "browser_specific_settings": { "gecko": { "id": "quiet@example.org" } } })"sv;
    auto first = TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "a.xpi"sv, quiet, ""sv)));
    // A new extension that asks for nothing installs without a question, but one that replaces another always asks,
    // since it takes over that extension's data.
    EXPECT(!WebView::install_needs_confirmation(first));
    (void)TRY_OR_FAIL(registry.try_install(move(first)));
    auto second = TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "b.xpi"sv, quiet, ""sv)));
    EXPECT(WebView::install_needs_confirmation(second));
    EXPECT_EQ(second.replaces, "Quiet"sv);
    registry.discard(move(second));
}

TEST_CASE(a_failed_reinstall_keeps_the_working_installation)
{
    TemporaryDirectory directory;
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(LexicalPath::join(directory.path, "profile"sv).string());
    auto first = TRY_OR_FAIL(registry.try_install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "v1.xpi"sv, gecko_manifest, "// one"sv)))));
    auto package = first->directory();
    auto pending = TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "v2.xpi"sv, gecko_manifest, "// two"sv)));
    // The staged files go bad before the install takes them, so loading them from the package fails.
    MUST(MUST(Core::File::open(LexicalPath::join(*pending.staged_package, "manifest.json"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate))->write_until_depleted("{"sv.bytes()));
    EXPECT(registry.try_install(move(pending)).is_error());
    EXPECT_EQ(read_text(LexicalPath::join(package, "bg.js"sv).string()), "// one"sv);
    EXPECT_EQ(settings.installed_extensions()[0].path, package);
    EXPECT_EQ(read_text(LexicalPath::join(registry.find(first->id())->directory(), "bg.js"sv).string()), "// one"sv);
    // Nothing is left beside the package.
    EXPECT_EQ(count_entries(LexicalPath(package).dirname()), 1u);
}

TEST_CASE(an_interrupted_reinstall_is_recovered_at_startup)
{
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    String id;
    ByteString package;
    {
        auto settings = WebView::Settings::create(directory.settings_path());
        ExtensionRegistry registry { settings };
        registry.set_profile_data_directory(profile);
        auto extension = TRY_OR_FAIL(registry.try_install(TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "v1.xpi"sv, gecko_manifest, "// one"sv)))));
        id = extension->id();
        package = extension->directory();
    }
    // A crash after the old package was moved aside and before the new one took its place leaves only the backup.
    MUST(FileSystem::rename_without_replacing(LexicalPath::join(LexicalPath(package).dirname(), ".backup-interrupted"sv).string(), package));
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(profile);
    registry.restore_from_settings();
    EXPECT(registry.find(id));
    EXPECT_EQ(read_text(LexicalPath::join(package, "bg.js"sv).string()), "// one"sv);
    EXPECT_EQ(count_entries(LexicalPath(package).dirname()), 1u);
}

TEST_CASE(a_declined_install_leaves_no_staging)
{
    TemporaryDirectory directory;
    auto profile = LexicalPath::join(directory.path, "profile"sv).string();
    auto settings = WebView::Settings::create(directory.settings_path());
    ExtensionRegistry registry { settings };
    registry.set_profile_data_directory(profile);
    auto pending = TRY_OR_FAIL(registry.prepare_packed(write_xpi(directory, "p.xpi"sv, gecko_manifest, ""sv)));
    auto staged = *pending.staged_package;
    EXPECT(FileSystem::exists(staged));
    registry.discard(move(pending));
    EXPECT(!FileSystem::exists(staged));
    EXPECT(settings.installed_extensions().is_empty());
}
