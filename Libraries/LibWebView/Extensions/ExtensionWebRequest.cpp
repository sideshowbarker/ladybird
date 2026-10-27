/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionWebRequest.h>

namespace WebView {

void ExtensionWebRequestRouter::notify_interest_changed()
{
    if (on_interest_changed)
        on_interest_changed();
}

void ExtensionWebRequestRouter::listeners_changed(Endpoint& endpoint, String const& extension_id, u64 context_id, Vector<Web::Extensions::WebRequestListener> listeners)
{
    ContextKey key { &endpoint, context_id };
    if (listeners.is_empty()) {
        if (m_contexts.remove(key))
            notify_interest_changed();
        return;
    }
    m_contexts.set(key, { extension_id, move(listeners) });
    notify_interest_changed();
}

void ExtensionWebRequestRouter::context_destroyed(Endpoint& endpoint, u64 context_id)
{
    if (m_contexts.remove({ &endpoint, context_id }))
        notify_interest_changed();
}

void ExtensionWebRequestRouter::endpoint_destroyed(Endpoint& endpoint)
{
    if (m_contexts.remove_all_matching([&](auto const& key, auto const&) { return key.endpoint == &endpoint; }))
        notify_interest_changed();
}

Vector<Requests::WebRequestInterest> ExtensionWebRequestRouter::interest() const
{
    Vector<Requests::WebRequestInterest> interest;
    for (auto const& [key, context] : m_contexts) {
        for (auto const& listener : context.listeners) {
            Requests::WebRequestInterest entry { listener.event, {}, listener.types, listener.blocking };
            for (auto const& url : listener.urls)
                entry.urls.append(url.to_string());
            if (!interest.contains_slow(entry))
                interest.append(move(entry));
        }
    }
    return interest;
}

String ExtensionWebRequestRouter::request_id_for(int client_id, u64 serial)
{
    return m_request_ids.ensure({ client_id, serial }, [&] { return String::number(++m_next_request_id); });
}

void ExtensionWebRequestRouter::forget_request(int client_id, u64 serial)
{
    m_request_ids.remove({ client_id, serial });
}

}
