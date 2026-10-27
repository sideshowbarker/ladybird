/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibURL/Parser.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

EventListenerList& MessagingContext::event_listeners(String const& event_name)
{
    if (auto listeners = m_event_listeners.get(event_name); listeners.has_value())
        return *listeners.value();
    auto listeners = heap().allocate<EventListenerList>();
    listeners->set_on_emptiness_change(GC::create_function(heap(), [this] { report_event_listeners(); }));
    m_event_listeners.set(event_name, listeners);
    return listeners;
}

void MessagingContext::report_event_listeners()
{
    // Any of the extension's own documents reports, since a background page may be one the manifest names, and have
    // frames; the UI process keeps only its background page's.
    if (is_detached())
        return;
    auto url = sender().url.has_value() ? URL::Parser::basic_parse(*sender().url) : Optional<URL::URL> {};
    if (!url.has_value() || url->scheme() != scheme || url->serialized_host() != extension_id())
        return;
    Vector<String> event_names;
    for (auto const& [name, listeners] : m_event_listeners) {
        if (!listeners->is_empty())
            event_names.append(name);
    }
    page().client().page_did_change_extension_event_listeners(extension_id(), context_id(), move(event_names));
}

GC::Ptr<EventListenerList> MessagingContext::existing_event_listeners(String const& event_name) const
{
    if (auto listeners = m_event_listeners.get(event_name); listeners.has_value())
        return listeners.value();
    return nullptr;
}

}
