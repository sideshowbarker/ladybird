/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> binary_path)
        : Test::HeadlessApplication(move(binary_path))
    {
    }

    // Test mode stays off: In it, a renderer may synthesize input, which the UI process would then take for user
    // activation — so the checks below that a request without activation is ignored would prove nothing.
    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }

    virtual void ask_user_for_extension_directory(Function<void(Optional<ByteString>)> callback) const override
    {
        ++picker_requests;
        callback({});
    }

    mutable size_t picker_requests { 0 };
};

}

TEST_CASE(extensions_user_activation)
{
    Test::HeadlessTestEnvironment environment { "TestExtensionsUserActivation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());
    VERIFY(!WebView::WebContentClient::renderers_may_synthesize_input());
    auto id = WebView::Application::settings().add_installed_extension(LexicalPath::join(environment.directory(), "missing-extension"sv).string());
    WebView::Application::extension_registry().restore_from_settings();

    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv).string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });
    view->load(URL::Parser::basic_parse("about:extensions"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 2; });

    bool ready = false;
    bool checked = false;
    view->on_title_change = [&](auto const& title) {
        if (title == "extensions-ready"sv)
            ready = true;
        if (title == "extensions-checked"sv)
            checked = true;
    };
    view->run_javascript(R"(
        function ready() {
            if (!document.querySelector('[data-extension-id]')) return false;
            document.title = 'extensions-ready';
            return true;
        }
        if (!ready()) {
            const observer = new MutationObserver(() => { if (ready()) observer.disconnect(); });
            observer.observe(document.documentElement, { childList: true, subtree: true });
        }
    )"_string);
    Core::EventLoop::current().spin_until([&] { return ready; });

    // A snapshot after both requests acknowledges their processing on the same WebUI connection.
    view->run_javascript(MUST(String::formatted(R"(
        document.addEventListener('WebUIMessage', function listener(event) {{
            if (event.detail.name !== 'extensionsSnapshot') return;
            document.removeEventListener('WebUIMessage', listener);
            document.title = 'extensions-checked';
        }});
        ladybird.sendMessage('removeExtension', {{ id: '{}' }});
        ladybird.sendMessage('loadUnpackedExtension', {{ directory: '/tmp' }});
        ladybird.sendMessage('loadExtensions');
    )",
        id)));
    Core::EventLoop::current().spin_until([&] { return checked; });
    VERIFY(WebView::Application::settings().installed_extensions().size() == 1);
    VERIFY(WebView::Application::settings().installed_extensions()[0].id == id);
    VERIFY(WebView::Application::extension_registry().failed_extensions().size() == 1);
    VERIFY(app->picker_requests == 0);
}
