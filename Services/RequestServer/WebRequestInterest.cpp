/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibURL/URL.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/WebRequestInterest.h>

namespace RequestServer {

WebRequestInterestSet& WebRequestInterestSet::the()
{
    static NeverDestroyed<WebRequestInterestSet> interest;
    return *interest;
}

void WebRequestInterestSet::set(Vector<Requests::WebRequestInterest> interest)
{
    m_entries.clear();
    for (auto& entry : interest) {
        Vector<Web::Extensions::MatchPattern> urls;
        for (auto const& url : entry.urls) {
            // The UI process sends only patterns that parsed in the extension's process; anything else matches nothing.
            if (auto pattern = Web::Extensions::MatchPattern::parse(url); !pattern.is_error())
                urls.append(pattern.release_value());
        }
        if (!urls.is_empty())
            m_entries.append({ entry.event, move(urls), move(entry.types), entry.blocking });
    }
}

WebRequestInterestSet::Entry const* WebRequestInterestSet::find(Requests::WebRequestEvent event, URL::URL const& url, StringView type, bool blocking_only) const
{
    for (auto const& entry : m_entries) {
        if (entry.event != event || (blocking_only && !entry.blocking))
            continue;
        if (!entry.types.is_empty() && !entry.types.contains_slow(type))
            continue;
        if (any_of(entry.urls, [&](auto const& pattern) { return pattern.matches(url); }))
            return &entry;
    }
    return nullptr;
}

bool WebRequestInterestSet::should_report(Requests::WebRequestEvent event, URL::URL const& url, StringView type) const
{
    return find(event, url, type, false);
}

bool WebRequestInterestSet::send(int client_id, u64 request_id, Requests::WebRequestEventDetails details, bool may_wait) const
{
    if (!should_report(details.event, details.url, details.context.type))
        return false;
    auto connection = ControlConnectionFromClient::the();
    if (!connection.has_value())
        return false;
    auto waits = may_wait && wants_answer(details.event, details.url, details.context.type);
    connection->async_web_request_event(client_id, request_id, move(details), waits);
    return waits;
}

bool WebRequestInterestSet::wants_answer(Requests::WebRequestEvent event, URL::URL const& url, StringView type) const
{
    return find(event, url, type, true);
}

}
