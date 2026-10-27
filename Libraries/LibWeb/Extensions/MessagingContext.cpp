/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

EventListenerList& MessagingContext::event_listeners(String const& event_name)
{
    if (auto listeners = m_event_listeners.get(event_name); listeners.has_value())
        return *listeners.value();
    auto listeners = heap().allocate<EventListenerList>();
    m_event_listeners.set(event_name, listeners);
    return listeners;
}

GC::Ptr<EventListenerList> MessagingContext::existing_event_listeners(String const& event_name) const
{
    if (auto listeners = m_event_listeners.get(event_name); listeners.has_value())
        return listeners.value();
    return nullptr;
}

}
