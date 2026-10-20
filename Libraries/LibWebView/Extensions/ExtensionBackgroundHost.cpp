/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionBackgroundHost.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace WebView {

ExtensionBackgroundHost::ExtensionBackgroundHost() = default;
ExtensionBackgroundHost::~ExtensionBackgroundHost() = default;

static URL::URL background_page_url(Extension const& extension)
{
    StringBuilder url;
    url.append(Web::Extensions::scheme);
    url.append("://"sv);
    url.append(extension.id());
    auto const& background = extension.manifest().background;
    if (background.has_value() && background->page.has_value()) {
        url.append('/');
        url.append(background->page->bytes_as_string_view().trim("/"sv, TrimMode::Left));
    } else {
        url.append(Web::Extensions::generated_background_page_path);
    }
    return URL::Parser::basic_parse(url.string_view()).value();
}

ErrorOr<Core::AnonymousBuffer> ExtensionBackgroundHost::theme()
{
    if (!m_theme.has_value()) {
        auto theme_path = LexicalPath::join(s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        m_theme = TRY(Gfx::load_system_theme(theme_path.string()));
    }
    return *m_theme;
}

void ExtensionBackgroundHost::prepare(ExtensionRegistry const& registry)
{
    Vector<String> removed;
    for (auto const& [id, revision] : m_instances) {
        auto extension = registry.find(id);
        if (!extension || !registry.is_enabled(id) || !extension->descriptor().has_background || extension->descriptor().revision != revision) {
            Application::extension_message_broker().background_finished(id, revision, false);
            removed.append(id);
        }
    }
    for (auto const& id : removed)
        m_instances.remove(id);
    for (auto const& extension : registry.extensions()) {
        if (registry.is_enabled(extension->id()) && extension->descriptor().has_background && !m_instances.contains(extension->id())) {
            m_instances.set(extension->id(), extension->descriptor().revision);
            Application::extension_message_broker().background_starting(extension->id(), extension->descriptor().revision);
        }
    }
}

void ExtensionBackgroundHost::sync(ExtensionRegistry const& registry)
{
    prepare(registry);
    Vector<String> ids_to_stop;
    for (auto const& entry : m_views) {
        auto extension = registry.find(entry.key);
        if (!extension || !registry.is_enabled(entry.key) || !extension->descriptor().has_background || extension->descriptor().revision != entry.value.revision)
            ids_to_stop.append(entry.key);
    }
    for (auto const& id : ids_to_stop)
        stop(id);

    for (auto const& extension : registry.extensions()) {
        if (registry.is_enabled(extension->id()) && extension->descriptor().has_background && !m_views.contains(extension->id()))
            start(*extension);
    }
}

void ExtensionBackgroundHost::start(Extension const& extension)
{
    auto theme = this->theme();
    if (theme.is_error()) {
        dbgln("Not starting the background page of extension {}: {}", extension.id(), theme.error());
        Application::extension_message_broker().background_finished(extension.id(), extension.descriptor().revision, false);
        return;
    }

    auto view = HeadlessWebView::create(theme.release_value(), { 800, 600 });
    auto url = background_page_url(extension);
    auto id = extension.id();
    auto revision = extension.descriptor().revision;
    auto startup_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [id, revision] {
        Application::extension_message_broker().background_finished(id, revision, false);
    });
    view->on_load_finish = [id, revision, url, startup_timer](auto const& loaded_url) {
        startup_timer->stop();
        Application::extension_message_broker().background_finished(id, revision, loaded_url == url);
    };
    view->on_web_content_crashed = [view = view.ptr(), id, revision, url, startup_timer](auto) {
        auto& broker = Application::extension_message_broker();
        broker.background_finished(id, revision, false);
        broker.background_starting(id, revision);
        startup_timer->restart();
        view->load(url);
    };
    startup_timer->start();
    view->load(url);
    m_views.set(id, Background { move(view), revision, move(startup_timer) });
}

void ExtensionBackgroundHost::stop(StringView extension_id)
{
    if (auto view = m_views.get(extension_id); view.has_value())
        Application::extension_message_broker().background_finished(MUST(String::from_utf8(extension_id)), view->revision, false);
    m_views.remove(extension_id);
}

bool ExtensionBackgroundHost::hosts_page(StringView extension_id, WebContentPage const& page) const
{
    auto view = m_views.get(extension_id);
    return view.has_value() && &view->view->page() == &page;
}

}
