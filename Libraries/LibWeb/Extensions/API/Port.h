/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Extensions/API/Event.h>

namespace Web::Extensions {

class MessagingContext;

class Port final : public GC::Cell {
    GC_CELL(Port, GC::Cell);
    GC_DECLARE_ALLOCATOR(Port);

public:
    static GC::Ref<Port> create(MessagingContext&, u64 port_id, u64 channel_id, String const& name, GC::Ptr<JS::Object> sender = nullptr);
    JS::Object& object() { return *m_object; }
    u64 channel_id() const { return m_channel_id; }
    void connected(u64 channel_id);
    void receive_message(String const& json);
    void receive_disconnect(Optional<String> const& error);
    void disconnect();

private:
    Port(MessagingContext&, u64 port_id, u64 channel_id);
    virtual void visit_edges(Visitor&) override;
    JS::ThrowCompletionOr<JS::Value> post_message(JS::Value);
    void send(String const& json);

    GC::Ref<MessagingContext> m_context;
    GC::Ref<JS::Realm> m_realm;
    GC::Ref<JS::Object> m_object;
    GC::Ref<EventListenerList> m_message_listeners;
    GC::Ref<EventListenerList> m_disconnect_listeners;
    JS::Value m_error { JS::js_null() };
    u64 m_port_id;
    u64 m_channel_id;
    Vector<String> m_queued_messages;
    bool m_disconnected { false };
    bool m_queue_overflow { false };
};

}
