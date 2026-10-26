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

static ExtensionBackgroundLifetime::Kind lifetime_kind(Extension const& extension)
{
    if (extension.manifest().background.has_value() && extension.manifest().background->persistent)
        return ExtensionBackgroundLifetime::Kind::Persistent;
    return extension.manifest().manifest_version == 2 ? ExtensionBackgroundLifetime::Kind::EventPage : ExtensionBackgroundLifetime::Kind::NonPersistentPage;
}

NonnullOwnPtr<ExtensionBackgroundHost::Background> ExtensionBackgroundHost::create_background(Extension const& extension)
{
    auto background = make<Background>();
    background->id = extension.id();
    background->revision = extension.descriptor().revision;
    background->url = background_page_url(extension);
    auto& record = *background;
    auto timer = [](Function<void()> fired) { return Core::Timer::create_single_shot(AK::Duration::zero(), move(fired)); };
    record.idle_timer = timer([&record] { record.lifetime->idle_timer_fired(); });
    record.suspend_timer = timer([&record] { record.lifetime->suspend_timer_fired(); });
    record.startup_timer = timer([this, &record] {
        Application::extension_message_broker().background_finished(record.id, record.revision, false);
        record.lifetime->loaded(false);
        destroy_view(record);
    });
    auto kind = lifetime_kind(extension);
    ExtensionBackgroundLifetime::Callbacks callbacks;
    callbacks.start = [this, &record] {
        Application::extension_message_broker().background_starting(record.id, record.revision);
        if (m_views_may_start)
            start_view(record);
    };
    // A page that never registered has no listeners to wait for, so it stops at once.
    // The page stops once every context of it has acknowledged; one that never registered has none to wait for.
    callbacks.suspend = [&record] {
        record.pending_suspend_acknowledgments.clear();
        for (auto context_id : Application::extension_message_broker().dispatch_suspend(record.id, false))
            record.pending_suspend_acknowledgments.set(context_id);
        if (record.pending_suspend_acknowledgments.is_empty())
            record.lifetime->suspend_acknowledged();
    };
    callbacks.stop = [this, &record] {
        record.pending_suspend_acknowledgments.clear();
        destroy_view(record);
    };
    callbacks.suspend_canceled = [&record] {
        record.pending_suspend_acknowledgments.clear();
        (void)Application::extension_message_broker().dispatch_suspend(record.id, true);
    };
    callbacks.arm_idle_timer = [&record](AK::Duration timeout) { record.idle_timer->restart(timeout); };
    callbacks.disarm_idle_timer = [&record] { record.idle_timer->stop(); };
    callbacks.arm_suspend_timer = [&record](AK::Duration timeout) { record.suspend_timer->restart(timeout); };
    callbacks.disarm_suspend_timer = [&record] { record.suspend_timer->stop(); };
    // An event page stays for as long as a message to it goes unanswered, or a port to it is open; a manifest version 3
    // page stays for at most max_keepalive per unanswered message, however much else happens meanwhile.
    callbacks.keepalive = [&record, kind] {
        auto& broker = Application::extension_message_broker();
        if (kind == ExtensionBackgroundLifetime::Kind::EventPage)
            return broker.has_unanswered_dispatches(record.id) || broker.has_open_channels(record.id);
        return broker.has_unanswered_dispatches(record.id, ExtensionBackgroundLifetime::max_keepalive);
    };
    record.lifetime = make<ExtensionBackgroundLifetime>(kind, move(callbacks));
    return background;
}

void ExtensionBackgroundHost::prepare(ExtensionRegistry const& registry)
{
    Vector<String> removed;
    for (auto const& [id, background] : m_backgrounds) {
        auto extension = registry.find(id);
        if (!extension || !registry.is_enabled(id) || !extension->descriptor().has_background || extension->descriptor().revision != background->revision)
            removed.append(id);
    }
    for (auto const& id : removed)
        forget(id);
    for (auto const& extension : registry.extensions()) {
        if (!registry.is_enabled(extension->id()) || !extension->descriptor().has_background || m_backgrounds.contains(extension->id()))
            continue;
        auto background = create_background(*extension);
        auto& record = *background;
        m_backgrounds.set(extension->id(), move(background));
        // Every page starts once, so that it can register its listeners.
        record.lifetime->wake();
    }
}

void ExtensionBackgroundHost::sync(ExtensionRegistry const& registry)
{
    prepare(registry);
    m_views_may_start = true;
    for (auto& [id, background] : m_backgrounds) {
        if (background->lifetime->state() == ExtensionBackgroundLifetime::State::Starting && !background->view)
            start_view(*background);
    }
}

bool ExtensionBackgroundHost::wake(StringView extension_id)
{
    auto background = m_backgrounds.get(extension_id);
    if (!background.has_value())
        return false;
    (*background)->lifetime->wake();
    return (*background)->lifetime->state() == ExtensionBackgroundLifetime::State::Starting;
}

void ExtensionBackgroundHost::activity(StringView extension_id, ExtensionBackgroundLifetime::FromPage from_page)
{
    if (auto background = m_backgrounds.get(extension_id); background.has_value())
        (*background)->lifetime->activity(from_page);
}

void ExtensionBackgroundHost::suspend_acknowledged(StringView extension_id, u64 context_id)
{
    auto background = m_backgrounds.get(extension_id);
    if (!background.has_value() || !(*background)->pending_suspend_acknowledgments.remove(context_id))
        return;
    if ((*background)->pending_suspend_acknowledgments.is_empty())
        (*background)->lifetime->suspend_acknowledged();
}

void ExtensionBackgroundHost::set_idle_timeout(StringView extension_id, AK::Duration timeout)
{
    if (auto background = m_backgrounds.get(extension_id); background.has_value())
        (*background)->lifetime->set_idle_timeout(timeout);
}

void ExtensionBackgroundHost::start_view(Background& record)
{
    auto theme = this->theme();
    if (theme.is_error()) {
        dbgln("Not starting the background page of extension {}: {}", record.id, theme.error());
        Application::extension_message_broker().background_finished(record.id, record.revision, false);
        record.lifetime->loaded(false);
        return;
    }

    record.view = HeadlessWebView::create(theme.release_value(), { 800, 600 });
    auto generation = ++record.view_generation;
    // The view's own callbacks can't destroy it, so they leave that to a task, which checks that the view it was
    // meant for is still the record's.
    auto destroy_later = [this, id = record.id, revision = record.revision, generation](bool restart) {
        Core::deferred_invoke([this, id, revision, generation, restart] {
            auto background = m_backgrounds.get(id);
            if (!background.has_value() || (*background)->revision != revision || (*background)->view_generation != generation)
                return;
            destroy_view(**background);
            if (restart)
                (*background)->lifetime->wake();
        });
    };
    // A page that changes its own fragment while it loads has still loaded the page the record names.
    record.view->on_load_finish = [&record, destroy_later](auto const& loaded_url) {
        if (record.lifetime->state() != ExtensionBackgroundLifetime::State::Starting)
            return;
        record.startup_timer->stop();
        auto success = loaded_url.equals(record.url, URL::ExcludeFragment::Yes);
        Application::extension_message_broker().background_finished(record.id, record.revision, success);
        record.lifetime->loaded(success);
        if (!success)
            destroy_later(false);
    };
    // A crashed page starts again at once if it's persistent, and otherwise when something next wakes it.
    record.view->on_web_content_crashed = [&record, destroy_later](auto) {
        Application::extension_message_broker().background_finished(record.id, record.revision, false);
        auto persistent = record.lifetime->kind() == ExtensionBackgroundLifetime::Kind::Persistent;
        record.lifetime->stopped();
        destroy_later(persistent);
    };
    record.startup_timer->restart(AK::Duration::from_seconds(30));
    record.view->load(record.url);
}

// Never called from one of the view's own callbacks: A crash reaches it through a deferred task instead.
void ExtensionBackgroundHost::destroy_view(Background& record)
{
    record.startup_timer->stop();
    // The page's contexts go with the view, so that nothing waits on them.
    if (record.view)
        Application::extension_message_broker().endpoint_gone(record.view->page());
    record.view = nullptr;
}

void ExtensionBackgroundHost::forget(String const& extension_id)
{
    auto background = m_backgrounds.take(extension_id);
    if (!background.has_value())
        return;
    Application::extension_message_broker().background_finished(extension_id, (*background)->revision, false);
    (*background)->lifetime->stopped();
    destroy_view(**background);
}

bool ExtensionBackgroundHost::is_hosting(StringView extension_id) const
{
    auto background = m_backgrounds.get(extension_id);
    return background.has_value() && (*background)->view;
}

bool ExtensionBackgroundHost::hosts_page(StringView extension_id, WebContentPage const& page) const
{
    auto background = m_backgrounds.get(extension_id);
    return background.has_value() && (*background)->view && &(*background)->view->page() == &page;
}

}
