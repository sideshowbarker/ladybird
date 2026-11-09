/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/ExtensionTabs.h>
#include <LibWebView/Extensions/ExtensionTabsAPI.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual Core::EventLoop& create_platform_event_loop() override { return Core::EventLoop::current(); }
    virtual bool should_coordinate_browser_process() const override { return false; }
};

class TestEnvironment {
public:
    TestEnvironment()
        : m_test_directory(ByteString::formatted("{}/Ladybird-TestExtensionTabs-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid()))
    {
    }

    ~TestEnvironment()
    {
        Core::ThreadEventQueue::destroy_current();
        if (FileSystem::exists(m_test_directory))
            (void)FileSystem::remove(m_test_directory, FileSystem::RecursionMode::Allowed);
    }

    ErrorOr<NonnullOwnPtr<TestApplication>> create_application()
    {
        auto make_test_dir = [&](auto dir, auto env) -> ErrorOr<void> {
            auto path = ByteString::formatted("{}/{}", m_test_directory, dir);
            TRY(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes));
            TRY(Core::Environment::set(env, path, Core::Environment::Overwrite::Yes));
            return {};
        };

        TRY(make_test_dir("config"sv, "XDG_CONFIG_HOME"sv));
        TRY(make_test_dir("data"sv, "XDG_DATA_HOME"sv));

        Main::Arguments arguments {
            .argc = 0,
            .argv = nullptr,
            .strings = Span<StringView> { &m_executable, 1 }
        };

#if defined(LADYBIRD_BINARY_PATH)
        return TestApplication::create(arguments, LADYBIRD_BINARY_PATH);
#else
        return TestApplication::create(arguments, OptionalNone {});
#endif
    }

private:
    Core::EventLoop m_event_loop;
    StringView m_executable { "ladybird"sv };
    ByteString m_test_directory;
};

}

static ErrorOr<Core::AnonymousBuffer> load_test_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return Gfx::load_system_theme(theme_path.string());
}

static Optional<WebView::ExtensionTab> tab_for_view(WebView::ViewImplementation const& view)
{
    for (auto const& tab : WebView::Application::the().extension_tabs()) {
        if (tab.view_id == view.view_id())
            return tab;
    }
    return {};
}

TEST_CASE(ids_are_never_reused)
{
    auto first_tab = WebView::allocate_extension_tab_id();
    auto second_tab = WebView::allocate_extension_tab_id();
    auto first_window = WebView::allocate_extension_window_id();
    auto second_window = WebView::allocate_extension_window_id();
    EXPECT(first_tab > 0);
    EXPECT(second_tab > first_tab);
    EXPECT(first_window > 0);
    EXPECT(second_window > first_window);
}

TEST_CASE(marked_views_are_tabs_of_their_own_windows)
{
    TestEnvironment environment;
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme = TRY_OR_FAIL(load_test_theme());
    auto first = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second = WebView::HeadlessWebView::create(theme, { 800, 600 }, WebView::IsPrivate::Yes);
    first->mark_as_extension_tab();
    second->mark_as_extension_tab();

    auto first_tab = tab_for_view(*first);
    auto second_tab = tab_for_view(*second);
    VERIFY(first_tab.has_value() && second_tab.has_value());
    EXPECT(first_tab->id != second_tab->id);
    EXPECT(first_tab->window_id != second_tab->window_id);
    EXPECT_EQ(first_tab->index, 0);
    EXPECT(first_tab->active);
    EXPECT(!first_tab->pinned);
    EXPECT_EQ(first_tab->is_private, WebView::IsPrivate::No);
    EXPECT_EQ(second_tab->is_private, WebView::IsPrivate::Yes);

    // Marking a view again keeps its ids.
    first->mark_as_extension_tab();
    EXPECT_EQ(tab_for_view(*first)->id, first_tab->id);
}

TEST_CASE(unmarked_views_are_not_tabs)
{
    TestEnvironment environment;
    auto app = TRY_OR_FAIL(environment.create_application());
    auto theme = TRY_OR_FAIL(load_test_theme());
    auto hidden = WebView::HeadlessWebView::create(theme, { 800, 600 });
    EXPECT(!tab_for_view(*hidden).has_value());
}

static WebView::ExtensionTab make_tab(i64 id, i64 window_id, i64 index, bool active, bool pinned, WebView::IsPrivate is_private, Optional<u64> view_id, StringView url, StringView title, bool loading, bool in_popup_window = false)
{
    return { id, window_id, index, active, pinned, is_private, view_id, URL::Parser::basic_parse(url).release_value(), MUST(String::from_utf8(title)), loading, in_popup_window };
}

static Vector<WebView::ExtensionTab> sample_tabs()
{
    using WebView::IsPrivate;
    return {
        make_tab(1, 1, 0, false, true, IsPrivate::No, 11, "https://a.example/one"sv, "One"sv, false),
        make_tab(2, 1, 1, true, false, IsPrivate::No, 12, "https://b.example/two"sv, "Two"sv, true),
        make_tab(3, 2, 0, true, false, IsPrivate::No, {}, "https://a.example/three"sv, "Three"sv, false),
        make_tab(4, 3, 0, true, false, IsPrivate::Yes, 14, "https://a.example/private"sv, "Private"sv, false),
    };
}

static WebView::ExtensionTabViewer viewer_for(Web::Extensions::ExtensionDescriptor const& extension, Optional<i64> current_window_id = {}, Optional<i64> last_focused_window_id = {}, bool allowed_in_private = false)
{
    return { extension, allowed_in_private, current_window_id, last_focused_window_id, nullptr };
}

static Web::Extensions::ExtensionDescriptor descriptor_with(Vector<String> api_permissions, Vector<StringView> host_patterns = {})
{
    Web::Extensions::ExtensionDescriptor descriptor;
    descriptor.id = "extension"_string;
    descriptor.api_permissions = move(api_permissions);
    for (auto pattern : host_patterns)
        descriptor.host_permissions.append(MUST(Web::Extensions::MatchPattern::parse(pattern)));
    return descriptor;
}

static Vector<i64> ids_of(JsonArray const& tabs)
{
    Vector<i64> ids;
    tabs.for_each([&](JsonValue const& tab) { ids.append(tab.as_object().get_integer<i64>("id"sv).value()); });
    return ids;
}

static JsonObject query_from(StringView json)
{
    return JsonValue::from_string(json).release_value().as_object();
}

TEST_CASE(query_filters_by_state)
{
    auto descriptor = descriptor_with({});
    auto viewer = viewer_for(descriptor, {}, 2);
    auto tabs = sample_tabs();
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{}"sv)))), (Vector<i64> { 1, 2, 3 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"active\":true}"sv)))), (Vector<i64> { 2, 3 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"pinned\":true}"sv)))), (Vector<i64> { 1 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"windowId\":1,\"index\":1}"sv)))), (Vector<i64> { 2 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"status\":\"loading\"}"sv)))), (Vector<i64> { 2 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"status\":\"unloaded\"}"sv)))), (Vector<i64> { 3 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"lastFocusedWindow\":true}"sv)))), (Vector<i64> { 3 }));
}

TEST_CASE(current_window_is_the_callers_or_the_last_focused)
{
    auto descriptor = descriptor_with({});
    auto tabs = sample_tabs();
    auto in_a_tab = viewer_for(descriptor, 1, 2);
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(in_a_tab, tabs, query_from("{\"currentWindow\":true}"sv)))), (Vector<i64> { 1, 2 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(in_a_tab, tabs, query_from("{\"windowId\":-2}"sv)))), (Vector<i64> { 1, 2 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(in_a_tab, tabs, query_from("{\"currentWindow\":false}"sv)))), (Vector<i64> { 3 }));
    auto in_the_background = viewer_for(descriptor, {}, 2);
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(in_the_background, tabs, query_from("{\"currentWindow\":true}"sv)))), (Vector<i64> { 3 }));
    auto with_no_windows = viewer_for(descriptor);
    EXPECT(TRY_OR_FAIL(WebView::query_extension_tabs(with_no_windows, tabs, query_from("{\"currentWindow\":true}"sv))).is_empty());
}

TEST_CASE(url_and_title_need_permission)
{
    auto tabs = sample_tabs();
    auto without = descriptor_with({});
    auto unprivileged = viewer_for(without);
    auto results = TRY_OR_FAIL(WebView::query_extension_tabs(unprivileged, tabs, query_from("{}"sv)));
    EXPECT(!results[0].as_object().has("url"sv));
    EXPECT(!results[0].as_object().has("title"sv));
    // A filter never matches what the caller can't see.
    EXPECT(TRY_OR_FAIL(WebView::query_extension_tabs(unprivileged, tabs, query_from("{\"url\":\"https://a.example/*\"}"sv))).is_empty());
    EXPECT(TRY_OR_FAIL(WebView::query_extension_tabs(unprivileged, tabs, query_from("{\"title\":\"One\"}"sv))).is_empty());

    auto with_tabs = descriptor_with({ "tabs"_string });
    auto privileged = viewer_for(with_tabs);
    results = TRY_OR_FAIL(WebView::query_extension_tabs(privileged, tabs, query_from("{\"url\":[\"https://a.example/*\"]}"sv)));
    EXPECT_EQ(ids_of(results), (Vector<i64> { 1, 3 }));
    EXPECT_EQ(results[0].as_object().get_string("url"sv), "https://a.example/one"_string);
    EXPECT_EQ(results[0].as_object().get_string("title"sv), "One"_string);
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(privileged, tabs, query_from("{\"title\":\"T*\"}"sv)))), (Vector<i64> { 2, 3 }));

    auto with_host = descriptor_with({}, { "https://b.example/*"sv });
    auto host_only = viewer_for(with_host);
    results = TRY_OR_FAIL(WebView::query_extension_tabs(host_only, tabs, query_from("{}"sv)));
    EXPECT(!results[0].as_object().has("url"sv));
    EXPECT_EQ(results[1].as_object().get_string("url"sv), "https://b.example/two"_string);
}

TEST_CASE(private_tabs_follow_the_opt_in)
{
    auto descriptor = descriptor_with({});
    auto tabs = sample_tabs();
    auto not_allowed = viewer_for(descriptor);
    EXPECT(!ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(not_allowed, tabs, query_from("{}"sv)))).contains_slow(4));
    auto allowed = viewer_for(descriptor, {}, {}, true);
    auto results = TRY_OR_FAIL(WebView::query_extension_tabs(allowed, tabs, query_from("{\"windowId\":3}"sv)));
    EXPECT_EQ(ids_of(results), (Vector<i64> { 4 }));
    EXPECT_EQ(results[0].as_object().get_bool("incognito"sv), true);
}

TEST_CASE(tab_shape)
{
    auto descriptor = descriptor_with({});
    auto viewer = viewer_for(descriptor);
    auto tab = WebView::serialize_extension_tab(viewer, sample_tabs()[1]);
    EXPECT_EQ(tab.get_integer<i64>("id"sv), 2);
    EXPECT_EQ(tab.get_integer<i64>("windowId"sv), 1);
    EXPECT_EQ(tab.get_integer<i64>("index"sv), 1);
    EXPECT_EQ(tab.get_bool("active"sv), true);
    EXPECT_EQ(tab.get_bool("highlighted"sv), true);
    EXPECT_EQ(tab.get_bool("selected"sv), true);
    EXPECT_EQ(tab.get_bool("pinned"sv), false);
    EXPECT_EQ(tab.get_bool("discarded"sv), false);
    EXPECT_EQ(tab.get_bool("incognito"sv), false);
    EXPECT_EQ(tab.get_string("status"sv), "loading"_string);
    EXPECT_EQ(tab.get_integer<i64>("groupId"sv), -1);
}

TEST_CASE(bad_url_patterns_are_errors)
{
    auto descriptor = descriptor_with({ "tabs"_string });
    auto viewer = viewer_for(descriptor);
    auto result = WebView::query_extension_tabs(viewer, sample_tabs(), query_from("{\"url\":\"bad\"}"sv));
    VERIFY(result.is_error());
    EXPECT_EQ(result.error(), "Invalid url pattern 'bad'"sv);
}

TEST_CASE(malformed_url_filters_are_errors)
{
    auto descriptor = descriptor_with({ "tabs"_string });
    auto viewer = viewer_for(descriptor);
    EXPECT(WebView::query_extension_tabs(viewer, sample_tabs(), query_from("{\"url\":42}"sv)).is_error());
    EXPECT(WebView::query_extension_tabs(viewer, sample_tabs(), query_from("{\"url\":[42]}"sv)).is_error());
}

TEST_CASE(empty_filters_and_negative_sentinels_restrict_nothing)
{
    auto descriptor = descriptor_with({ "tabs"_string });
    auto viewer = viewer_for(descriptor);
    auto tabs = sample_tabs();
    for (auto query : { "{\"url\":[]}"sv, "{\"title\":\"\"}"sv, "{\"index\":-1}"sv, "{\"windowId\":-1}"sv })
        EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from(query)))), (Vector<i64> { 1, 2, 3 }));
}

TEST_CASE(window_type_tells_popups_apart)
{
    auto descriptor = descriptor_with({});
    auto viewer = viewer_for(descriptor);
    auto tabs = sample_tabs();
    tabs.append(make_tab(5, 4, 0, true, false, WebView::IsPrivate::No, 15, "https://c.example/"sv, "Popup"sv, false, true));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"windowType\":\"popup\"}"sv)))), (Vector<i64> { 5 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"windowType\":\"normal\"}"sv)))), (Vector<i64> { 1, 2, 3 }));
}

TEST_CASE(an_extension_sees_its_own_pages)
{
    auto descriptor = descriptor_with({});
    auto viewer = viewer_for(descriptor);
    auto tabs = sample_tabs();
    tabs.append(make_tab(5, 1, 2, false, false, WebView::IsPrivate::No, 15, "ladybird-extension://extension/options.html"sv, "Options"sv, false));
    auto results = TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"url\":\"ladybird-extension://extension/*\"}"sv)));
    EXPECT_EQ(ids_of(results), (Vector<i64> { 5 }));
    EXPECT_EQ(results[0].as_object().get_string("title"sv), "Options"_string);
}

TEST_CASE(title_wildcards_match_characters)
{
    auto descriptor = descriptor_with({ "tabs"_string });
    auto viewer = viewer_for(descriptor);
    Vector<WebView::ExtensionTab> tabs { make_tab(1, 1, 0, true, false, WebView::IsPrivate::No, 11, "https://a.example/"sv, "\xe7\x8c\xab"sv, false), make_tab(2, 1, 1, false, false, WebView::IsPrivate::No, 12, "https://b.example/"sv, "ab"sv, false) };
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"title\":\"?\"}"sv)))), (Vector<i64> { 1 }));
    EXPECT_EQ(ids_of(TRY_OR_FAIL(WebView::query_extension_tabs(viewer, tabs, query_from("{\"title\":\"a?b\"}"sv)))), (Vector<i64> { 2 }));
}

TEST_CASE(last_focused_window_skips_windows_the_extension_cant_see)
{
    auto tabs = sample_tabs();
    Vector<i64> focus_order { 3, 2, 1 };
    EXPECT_EQ(WebView::last_focused_extension_window_for(focus_order, tabs, false), 2);
    EXPECT_EQ(WebView::last_focused_extension_window_for(focus_order, tabs, true), 3);
    EXPECT(!WebView::last_focused_extension_window_for({}, tabs, true).has_value());
}

TEST_CASE(active_tab_grant_ends_at_another_origin)
{
    WebView::ExtensionActiveTabGrants grants;
    auto page = URL::Parser::basic_parse("https://example.com/a"sv).release_value();
    grants.grant(1, page);
    EXPECT(grants.contains(1));
    EXPECT(!grants.contains(2));

    // The grant follows the tab across its own origin, and ends at another one or when the tab closes.
    grants.tab_navigated(1, URL::Parser::basic_parse("https://example.com/b"sv).release_value());
    EXPECT(grants.contains(1));
    grants.tab_navigated(1, URL::Parser::basic_parse("https://other.example/"sv).release_value());
    EXPECT(!grants.contains(1));
    grants.grant(1, page);
    grants.tab_closed(1);
    EXPECT(!grants.contains(1));
}

TEST_CASE(tab_urls_resolve_against_the_extension_and_reject_what_extensions_cannot_open)
{
    auto extension = descriptor_with({});
    auto resolve = [&](StringView url) { return WebView::resolve_extension_tab_url(extension, url); };

    EXPECT_EQ(MUST(resolve("popup.html"sv)).serialize(), "ladybird-extension://extension/popup.html"sv);
    EXPECT_EQ(MUST(resolve("/a/b.html?x"sv)).serialize(), "ladybird-extension://extension/a/b.html?x"sv);
    EXPECT_EQ(MUST(resolve("https://example.com/"sv)).serialize(), "https://example.com/"sv);
    EXPECT_EQ(MUST(resolve("data:text/plain,x"sv)).serialize(), "data:text/plain,x"sv);
    EXPECT_EQ(MUST(resolve("ladybird-extension://extension/page.html"sv)).serialize(), "ladybird-extension://extension/page.html"sv);

    EXPECT_EQ(resolve("javascript:1"sv).error(), "JavaScript URLs are not allowed in API based extension navigations. Use chrome.scripting.executeScript instead."sv);
    EXPECT_EQ(resolve("file:///etc/hosts"sv).error(), "Cannot navigate to a file URL without local file access."sv);
    EXPECT_EQ(resolve("ladybird-extension://other/page.html"sv).error(), "Cannot navigate to another extension's page."sv);
    EXPECT_EQ(resolve("http://[::1"sv).error(), "Invalid url: \"http://[::1\"."sv);
}

static WebView::ExtensionTab tab_with(StringView url, StringView title, bool loading, WebView::IsPrivate is_private = WebView::IsPrivate::No)
{
    return { .id = 7, .window_id = 3, .index = 0, .active = true, .pinned = false, .is_private = is_private, .view_id = 1, .url = URL::Parser::basic_parse(url).value(), .title = MUST(String::from_utf8(title)), .loading = loading, .in_popup_window = false };
}

TEST_CASE(tab_events_follow_the_redaction_rule)
{
    auto with_tabs = descriptor_with({ "tabs"_string });
    auto without = descriptor_with({});
    auto before = tab_with("https://example.com/"sv, "Old"sv, false);
    auto after = tab_with("https://example.com/"sv, "New"sv, false);

    auto seen = WebView::extension_tab_change_info(viewer_for(with_tabs), before, after);
    EXPECT(seen.has_value());
    EXPECT_EQ(seen->serialized(), "{\"title\":\"New\"}"sv);
    EXPECT(!WebView::extension_tab_change_info(viewer_for(without), before, after).has_value());

    // Pinning shows no URL or title, so every extension sees it.
    auto pinned = after;
    pinned.pinned = true;
    EXPECT_EQ(WebView::extension_tab_change_info(viewer_for(without), after, pinned)->serialized(), "{\"pinned\":true}"sv);
}

TEST_CASE(a_load_reports_loading_with_its_url_then_complete)
{
    auto extension = descriptor_with({ "tabs"_string });
    auto idle = tab_with("https://example.com/"sv, "Title"sv, false);
    auto loading = tab_with("https://example.org/"sv, "Title"sv, true);
    auto complete = tab_with("https://example.org/"sv, "Title"sv, false);
    EXPECT_EQ(WebView::extension_tab_change_info(viewer_for(extension), idle, loading)->serialized(), "{\"status\":\"loading\",\"url\":\"https://example.org/\"}"sv);
    EXPECT_EQ(WebView::extension_tab_change_info(viewer_for(extension), loading, complete)->serialized(), "{\"status\":\"complete\"}"sv);
    EXPECT(!WebView::extension_tab_change_info(viewer_for(extension), complete, complete).has_value());

    // Without details access, the status still shows, but not the URL.
    EXPECT_EQ(WebView::extension_tab_change_info(viewer_for(descriptor_with({})), idle, loading)->serialized(), "{\"status\":\"loading\"}"sv);
}

TEST_CASE(a_private_tab_has_no_events_for_an_extension_not_allowed_there)
{
    auto extension = descriptor_with({ "tabs"_string });
    auto before = tab_with("https://example.com/"sv, "Old"sv, false, WebView::IsPrivate::Yes);
    auto after = tab_with("https://example.com/"sv, "New"sv, true, WebView::IsPrivate::Yes);
    EXPECT(!WebView::extension_tab_change_info(viewer_for(extension), before, after).has_value());
    EXPECT(WebView::extension_tab_change_info(viewer_for(extension, {}, {}, true), before, after).has_value());
}

TEST_CASE(focus_changes_dispatch_once)
{
    WebView::ExtensionWindowFocus focus;
    // The first window to gain focus is a change.
    EXPECT_EQ(focus.focus_changed(3), Optional<i64> { 3 });
    // Focus staying where it is changes nothing.
    EXPECT_EQ(focus.focus_changed(3), Optional<i64> {});
    EXPECT_EQ(focus.focus_changed(5), Optional<i64> { 5 });
    // No window of the browser having focus is WINDOW_ID_NONE, once.
    EXPECT_EQ(focus.focus_changed(WebView::ExtensionWindowFocus::no_window), Optional<i64> { -1 });
    EXPECT_EQ(focus.focus_changed(WebView::ExtensionWindowFocus::no_window), Optional<i64> {});
    EXPECT_EQ(focus.focus_changed(5), Optional<i64> { 5 });
}
