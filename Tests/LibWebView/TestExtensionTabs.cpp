/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/ExtensionTabs.h>
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
