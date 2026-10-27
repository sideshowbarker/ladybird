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

    virtual void ask_user_for_extension_file(Function<void(Optional<ByteString>)> callback) const override
    {
        ++picker_requests;
        callback({});
    }

    virtual void open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab) const override { opened_urls.append(url); }

    mutable size_t picker_requests { 0 };
    mutable Vector<URL::URL> opened_urls;
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
        ladybird.sendMessage('openExtensionOptions', {{ id: '{}' }});
        ladybird.sendMessage('installExtensionFromFile');
        ladybird.sendMessage('loadExtensions');
    )",
        id, id)));
    Core::EventLoop::current().spin_until([&] { return checked; });
    VERIFY(WebView::Application::settings().installed_extensions().size() == 1);
    VERIFY(WebView::Application::settings().installed_extensions()[0].id == id);
    VERIFY(WebView::Application::extension_registry().failed_extensions().size() == 1);
    VERIFY(app->picker_requests == 0);
    size_t views = 0;
    WebView::ViewImplementation::for_each_view([&](auto&) {
        ++views;
        return IterationDecision::Continue;
    });
    VERIFY(views == 1);

    // The Show in toolbar switch needs no click to be read, so its message sets the extension's setting at once.
    bool switched = false;
    view->on_title_change = [&](auto const& title) {
        if (title == "extensions-switched"sv)
            switched = true;
    };
    view->run_javascript(MUST(String::formatted(R"(
        document.addEventListener('WebUIMessage', function listener(event) {{
            if (event.detail.name !== 'extensionsSnapshot') return;
            document.removeEventListener('WebUIMessage', listener);
            document.title = 'extensions-switched';
        }});
        ladybird.sendMessage('setExtensionShownInToolbar', {{ id: '{}', shown: false }});
        ladybird.sendMessage('loadExtensions');
    )",
        id)));
    Core::EventLoop::current().spin_until([&] { return switched; });
    VERIFY(WebView::Application::settings().installed_extensions()[0].hidden_in_toolbar);

    // Manage extension opens about:extensions at the extension's card, which the page highlights.
    app->open_extension_management(id);
    VERIFY(app->opened_urls.size() == 1);
    VERIFY(app->opened_urls[0].serialize() == MUST(String::formatted("about:extensions#{}", id)));
    bool highlighted = false;
    view->on_title_change = [&](auto const& title) {
        if (title == "extensions-highlighted"sv)
            highlighted = true;
    };
    view->load(app->opened_urls[0]);
    view->run_javascript(MUST(String::formatted(R"(
        function highlighted() {{
            const card = document.querySelector('[data-extension-id="{}"]');
            if (!card || !card.classList.contains('highlighted')) return false;
            document.title = 'extensions-highlighted';
            return true;
        }}
        if (!highlighted()) {{
            const observer = new MutationObserver(() => {{ if (highlighted()) observer.disconnect(); }});
            observer.observe(document.documentElement, {{ attributes: true, childList: true, subtree: true }});
        }}
    )",
        id)));
    Core::EventLoop::current().spin_until([&] { return highlighted; });

    // about:settings links to about:extensions from an Extensions card after Content.
    bool settings_checked = false;
    view->on_title_change = [&](auto const& title) {
        if (title == "settings-checked"sv)
            settings_checked = true;
    };
    size_t settings_loads = loads_finished;
    view->load(URL::Parser::basic_parse("about:settings"sv).release_value());
    Core::EventLoop::current().spin_until([&] { return loads_finished > settings_loads; });
    view->run_javascript(R"(
        const titles = [...document.querySelectorAll('.card-title')].map(title => title.textContent);
        const link = document.getElementById('manage-extensions');
        if (titles.indexOf('Extensions') === titles.indexOf('Content') + 1 && link && link.getAttribute('href') === 'about:extensions')
            document.title = 'settings-checked';
    )"_string);
    Core::EventLoop::current().spin_until([&] { return settings_checked; });
}
