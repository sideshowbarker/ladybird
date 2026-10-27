/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionPopupHost.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

static constexpr Gfx::IntSize minimum_popup_size { 25, 25 };
static constexpr Gfx::IntSize maximum_popup_size { 800, 600 };

Gfx::IntSize clamp_extension_popup_size(Gfx::IntSize size)
{
    return { clamp(size.width(), minimum_popup_size.width(), maximum_popup_size.width()),
        clamp(size.height(), minimum_popup_size.height(), maximum_popup_size.height()) };
}

ExtensionPopupHost::ExtensionPopupHost(Frontend& frontend)
    : m_frontend(frontend)
{
}

void ExtensionPopupHost::toggle(ExtensionPopupRequest request)
{
    auto window_id = request.window_id;
    if (auto popup = m_popups.get(window_id); popup.has_value()) {
        auto same_extension = popup->request.extension_id == request.extension_id;
        close(window_id, same_extension ? CloseReason::Toggled : CloseReason::Replaced);
        if (same_extension)
            return;
    }
    request.generation = m_next_generation++;
    if (!m_frontend.show_extension_popup(request))
        return;
    m_popups.set(window_id, { move(request), {} });
}

void ExtensionPopupHost::close(i64 window_id, CloseReason)
{
    if (!m_popups.remove(window_id))
        return;
    m_frontend.close_extension_popup(window_id);
}

void ExtensionPopupHost::close(i64 window_id, u64 generation, CloseReason reason)
{
    if (auto popup = m_popups.get(window_id); popup.has_value() && popup->request.generation == generation)
        close(window_id, reason);
}

void ExtensionPopupHost::content_size_changed(i64 window_id, u64 generation, Gfx::IntSize content_size)
{
    auto popup = m_popups.find(window_id);
    if (popup == m_popups.end() || popup->value.request.generation != generation)
        return;
    auto size = clamp_extension_popup_size(content_size);
    if (popup->value.size == size)
        return;
    popup->value.size = size;
    m_frontend.resize_extension_popup(window_id, size);
}

void ExtensionPopupHost::extension_gone(StringView extension_id)
{
    Vector<i64> windows;
    for (auto const& [window_id, popup] : m_popups) {
        if (popup.request.extension_id == extension_id)
            windows.append(window_id);
    }
    for (auto window_id : windows)
        close(window_id, CloseReason::ExtensionGone);
}

void ExtensionPopupHost::tab_closed(i64 tab_id)
{
    Vector<i64> windows;
    for (auto const& [window_id, popup] : m_popups) {
        if (popup.request.tab_id == tab_id)
            windows.append(window_id);
    }
    for (auto window_id : windows)
        close(window_id, CloseReason::TabClosed);
}

Optional<String> ExtensionPopupHost::extension_in(i64 window_id) const
{
    if (auto popup = m_popups.get(window_id); popup.has_value())
        return popup->request.extension_id;
    return {};
}

Optional<i64> ExtensionPopupHost::window_showing(WebContentPage const& page) const
{
    // Only the page showing the popup's top-level document is the popup's; its iframes' pages aren't.
    auto const* traversable = page.traversable_if_alive();
    if (!traversable || traversable->display_page().ptr() != &page)
        return {};
    auto view = traversable->view();
    if (!view.has_value())
        return {};
    for (auto const& [window_id, popup] : m_popups) {
        if (m_frontend.extension_popup_view(window_id) == &view.value())
            return window_id;
    }
    return {};
}

Optional<i64> ExtensionPopupHost::window_for_page(WebContentPage const& page) const
{
    return window_showing(page);
}

bool ExtensionPopupHost::hosts_page(StringView extension_id, WebContentPage const& page) const
{
    auto window_id = window_showing(page);
    return window_id.has_value() && m_popups.get(*window_id)->request.extension_id == extension_id;
}

}
