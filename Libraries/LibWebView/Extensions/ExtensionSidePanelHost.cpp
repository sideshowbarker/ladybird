/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionSidePanelHost.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

ExtensionSidePanelHost::ExtensionSidePanelHost(Frontend& frontend)
    : m_frontend(frontend)
{
}

ExtensionSidePanelHost::Entry const* ExtensionSidePanelHost::find(StringView id) const
{
    for (auto const& entry : m_entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

ExtensionSidePanelHost::Entry* ExtensionSidePanelHost::find(StringView id)
{
    return const_cast<Entry*>(static_cast<ExtensionSidePanelHost const&>(*this).find(id));
}

// A manifest path is extension-relative; the options hold it with a leading slash, as the API does.
static String absolute_path(StringView path)
{
    if (path.starts_with('/'))
        return MUST(String::from_utf8(path));
    return MUST(String::formatted("/{}", path));
}

void ExtensionSidePanelHost::set_extension(String id, String name, ExtensionManifest::SidePanel declaration)
{
    remove_extension(id);
    ExtensionSidePanelOptions global { absolute_path(declaration.path), true };
    m_entries.append({ move(id), move(name), move(declaration), move(global), {}, false });
}

void ExtensionSidePanelHost::remove_extension(StringView id)
{
    close_panels_of(id, CloseReason::ExtensionGone);
    m_entries.remove_all_matching([&](auto const& entry) { return entry.id == id; });
}

void ExtensionSidePanelHost::close_panels_of(StringView id, CloseReason reason)
{
    Vector<i64> windows;
    for (auto const& [window_id, panel] : m_panels) {
        if (panel.extension_id == id)
            windows.append(window_id);
    }
    for (auto window_id : windows)
        close(window_id, reason);
}

bool ExtensionSidePanelHost::has_side_panel(StringView id) const
{
    return find(id) != nullptr;
}

void ExtensionSidePanelHost::set_options(StringView id, Optional<i64> tab_id, Optional<String> path, Optional<bool> enabled, OverrideSource source)
{
    auto* entry = find(id);
    if (!entry)
        return;
    if (!tab_id.has_value()) {
        if (path.has_value())
            entry->global.path = absolute_path(*path);
        if (enabled.has_value())
            entry->global.enabled = *enabled;
    } else {
        auto& override = entry->per_tab.ensure(*tab_id);
        if (path.has_value())
            override.path = absolute_path(*path);
        if (enabled.has_value())
            override.enabled = *enabled;
        override.source = source;
    }
    resolve_open_panels_showing(id);
}

ExtensionSidePanelOptions ExtensionSidePanelHost::options(StringView id, Optional<i64> tab_id) const
{
    auto const* entry = find(id);
    if (!entry)
        return { {}, false };
    auto options = entry->global;
    if (!tab_id.has_value())
        return options;
    if (auto override = entry->per_tab.get(*tab_id); override.has_value()) {
        if (override->path.has_value())
            options.path = override->path;
        if (override->enabled.has_value())
            options.enabled = *override->enabled;
    }
    return options;
}

void ExtensionSidePanelHost::set_opens_on_action_click(StringView id, bool opens)
{
    if (auto* entry = find(id))
        entry->opens_on_action_click = opens;
}

bool ExtensionSidePanelHost::opens_on_action_click(StringView id) const
{
    auto const* entry = find(id);
    return entry && entry->opens_on_action_click;
}

URL::URL ExtensionSidePanelHost::url_for(StringView id, StringView path) const
{
    return URL::Parser::basic_parse(MUST(String::formatted("{}://{}{}", Web::Extensions::scheme, id, path))).release_value();
}

// The page the window's panel shows for this extension: the active tab's override, else the global page; nothing
// where the panel is disabled or has no page.
Optional<URL::URL> ExtensionSidePanelHost::resolved_url(Entry const& entry, i64 window_id) const
{
    auto options = this->options(entry.id, m_frontend.extension_active_tab_of_window(window_id));
    if (!options.enabled || !options.path.has_value())
        return {};
    return url_for(entry.id, *options.path);
}

void ExtensionSidePanelHost::show(Entry const& entry, i64 window_id, Optional<i64> tab_id, URL::URL url)
{
    ExtensionSidePanelRequest request { entry.id, window_id, url, entry.declaration.title.value_or(entry.name), entry.declaration.icon, m_next_generation++ };
    if (!m_frontend.show_extension_side_panel(request))
        return;
    m_panels.set(window_id, { entry.id, move(url), tab_id, request.generation });
    if (on_opened)
        on_opened(entry.id, window_id, tab_id, MUST(String::from_utf8(request.url.serialize_path())));
}

ErrorOr<void, String> ExtensionSidePanelHost::open(StringView id, Optional<i64> window_id, Optional<i64> tab_id)
{
    if (!window_id.has_value() && !tab_id.has_value())
        return "At least one of `tabId` and `windowId` must be provided"_string;
    if (tab_id.has_value()) {
        auto tab_window = m_frontend.extension_window_of_tab(*tab_id);
        if (!tab_window.has_value())
            return MUST(String::formatted("No tab with id: {}.", *tab_id));
        if (window_id.has_value() && *window_id != *tab_window)
            return "The specified tab does not belong to the specified window."_string;
        window_id = tab_window;
    }
    auto const* entry = find(id);
    if (!m_frontend.extension_active_tab_of_window(*window_id).has_value() || (entry && !m_frontend.extension_may_show_in_window(id, *window_id)))
        return MUST(String::formatted("No window with id: {}.", *window_id));

    auto options = this->options(id, tab_id.has_value() ? tab_id : m_frontend.extension_active_tab_of_window(*window_id));
    if (!entry || !options.enabled || !options.path.has_value()) {
        if (tab_id.has_value())
            return MUST(String::formatted("No active side panel for tabId: {}", *tab_id));
        return MUST(String::formatted("No active side panel for windowId: {}", *window_id));
    }

    auto url = url_for(id, *options.path);
    if (auto panel = m_panels.get(*window_id); panel.has_value()) {
        if (panel->extension_id == id) {
            if (panel->url != url) {
                m_panels.get(*window_id)->url = url;
                m_frontend.navigate_extension_side_panel(*window_id, url);
            }
            return {};
        }
        close(*window_id, CloseReason::Replaced);
    }
    show(*entry, *window_id, tab_id, move(url));
    return {};
}

ErrorOr<void, String> ExtensionSidePanelHost::close(StringView id, Optional<i64> window_id, Optional<i64> tab_id)
{
    if (!window_id.has_value() && !tab_id.has_value())
        return "At least one of `tabId` and `windowId` must be provided"_string;
    if (tab_id.has_value()) {
        auto tab_window = m_frontend.extension_window_of_tab(*tab_id);
        if (!tab_window.has_value())
            return MUST(String::formatted("No tab with id: {}.", *tab_id));
        if (window_id.has_value() && *window_id != *tab_window)
            return "The specified tab does not belong to the specified window."_string;
        window_id = tab_window;
    }
    if (!m_frontend.extension_active_tab_of_window(*window_id).has_value())
        return MUST(String::formatted("No window with id: {}.", *window_id));
    if (is_open(id, *window_id))
        close(*window_id, CloseReason::Api);
    return {};
}

void ExtensionSidePanelHost::toggle(StringView id, i64 window_id)
{
    if (is_open(id, window_id)) {
        close(window_id, CloseReason::Api);
        return;
    }
    (void)open(id, window_id, {});
}

void ExtensionSidePanelHost::close(i64 window_id, CloseReason)
{
    auto panel = m_panels.take(window_id);
    if (!panel.has_value())
        return;
    m_frontend.close_extension_side_panel(window_id);
    if (on_closed)
        on_closed(panel->extension_id, window_id, panel->tab_id, MUST(String::from_utf8(panel->url.serialize_path())));
}

void ExtensionSidePanelHost::close(i64 window_id, u64 generation, CloseReason reason)
{
    if (auto panel = m_panels.get(window_id); panel.has_value() && panel->generation == generation)
        close(window_id, reason);
}

bool ExtensionSidePanelHost::is_open(StringView id, i64 window_id) const
{
    auto panel = m_panels.get(window_id);
    return panel.has_value() && panel->extension_id == id;
}

Optional<String> ExtensionSidePanelHost::extension_in(i64 window_id) const
{
    if (auto panel = m_panels.get(window_id); panel.has_value())
        return panel->extension_id;
    return {};
}

// Every open panel of the extension shows what its window's active tab resolves to now.
void ExtensionSidePanelHost::resolve_open_panels_showing(StringView id)
{
    auto const* entry = find(id);
    Vector<i64> windows;
    for (auto const& [window_id, panel] : m_panels) {
        if (panel.extension_id == id)
            windows.append(window_id);
    }
    for (auto window_id : windows) {
        auto url = entry ? resolved_url(*entry, window_id) : Optional<URL::URL> {};
        if (!url.has_value()) {
            close(window_id, CloseReason::Disabled);
            continue;
        }
        auto& panel = *m_panels.get(window_id);
        if (panel.url == *url)
            continue;
        panel.url = *url;
        m_frontend.navigate_extension_side_panel(window_id, *url);
    }
}

void ExtensionSidePanelHost::active_tab_changed(i64 window_id, i64)
{
    auto panel = m_panels.get(window_id);
    if (!panel.has_value())
        return;
    resolve_open_panels_showing(panel->extension_id);
}

void ExtensionSidePanelHost::tab_closed(i64 tab_id)
{
    for (auto& entry : m_entries) {
        if (entry.per_tab.remove(tab_id))
            resolve_open_panels_showing(entry.id);
    }
}

void ExtensionSidePanelHost::tab_navigated(i64 tab_id)
{
    for (auto& entry : m_entries) {
        auto override = entry.per_tab.get(tab_id);
        if (!override.has_value() || override->source != OverrideSource::SidebarAction)
            continue;
        entry.per_tab.remove(tab_id);
        resolve_open_panels_showing(entry.id);
    }
}

Optional<ExtensionSidePanelHost::PanelState> ExtensionSidePanelHost::state(i64 window_id) const
{
    auto panel = m_panels.get(window_id);
    if (!panel.has_value())
        return {};
    auto const* entry = find(panel->extension_id);
    return PanelState { panel->extension_id, panel->url, entry ? entry->declaration.title.value_or(entry->name) : String {} };
}

Optional<i64> ExtensionSidePanelHost::window_showing(WebContentPage const& page) const
{
    // Only the page showing the panel's top-level document is the panel's; its iframes' pages aren't.
    auto const* traversable = page.traversable_if_alive();
    if (!traversable || traversable->display_page().ptr() != &page)
        return {};
    auto view = traversable->view();
    if (!view.has_value())
        return {};
    for (auto const& [window_id, panel] : m_panels) {
        if (m_frontend.extension_side_panel_view(window_id) == &view.value())
            return window_id;
    }
    return {};
}

Optional<i64> ExtensionSidePanelHost::window_for_page(WebContentPage const& page) const
{
    return window_showing(page);
}

bool ExtensionSidePanelHost::hosts_page(StringView extension_id, WebContentPage const& page) const
{
    auto window_id = window_showing(page);
    return window_id.has_value() && m_panels.get(*window_id)->extension_id == extension_id;
}

}
