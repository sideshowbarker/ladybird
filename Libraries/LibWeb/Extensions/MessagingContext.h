/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Port.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/Extensions/MessageSender.h>

namespace Web::Extensions {

// A JavaScript context of one extension that can send and receive runtime messages: a content script's world in a
// document, or one of the extension's own documents. The router keeps the live ones, by extension id.
class WEB_API MessagingContext : public GC::Cell {
    GC_CELL(MessagingContext, GC::Cell);

public:
    virtual ~MessagingContext() override = default;

    JS::Value last_error() const { return m_last_error; }
    void set_last_error(JS::Value value) { m_last_error = value; }

    void set_storage_listeners(GC::Ref<EventListenerList> global, GC::Ref<EventListenerList> local)
    {
        m_storage_listeners = global;
        m_local_storage_listeners = local;
    }
    GC::Ptr<EventListenerList> storage_listeners() { return m_storage_listeners; }
    GC::Ptr<EventListenerList> local_storage_listeners() { return m_local_storage_listeners; }
    void revoke_storage_listeners()
    {
        if (m_storage_listeners)
            m_storage_listeners->revoke();
        if (m_local_storage_listeners)
            m_local_storage_listeners->revoke();
    }

    void set_connect_listeners(GC::Ref<EventListenerList> listeners) { m_connect_listeners = listeners; }
    GC::Ptr<EventListenerList> connect_listeners() { return m_connect_listeners; }
    HashMap<u64, GC::Ref<Port>>& live_ports() { return m_live_ports; }

    u64 context_id() const { return m_context_id; }
    virtual DOM::Document& document() = 0;

    virtual JS::Realm& realm() = 0;
    virtual ExtensionDescriptor const& descriptor() const = 0;
    virtual EventListenerList& message_listeners() = 0;
    virtual Page& page() = 0;
    virtual MessageSender sender() const = 0;
    virtual bool is_detached() const = 0;
    virtual void detach() = 0;

    String const& extension_id() const { return descriptor().id; }

protected:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_connect_listeners);
        for (auto const& [id, port] : m_live_ports)
            visitor.visit(port);
        visitor.visit(m_last_error);
        visitor.visit(m_storage_listeners);
        visitor.visit(m_local_storage_listeners);
    }

    MessagingContext()
        : m_context_id(++s_next_context_id)
    {
    }

private:
    inline static u64 s_next_context_id { 0 };
    u64 m_context_id;
    GC::Ptr<EventListenerList> m_connect_listeners;
    HashMap<u64, GC::Ref<Port>> m_live_ports;
    GC::Ptr<EventListenerList> m_storage_listeners;
    GC::Ptr<EventListenerList> m_local_storage_listeners;
    JS::Value m_last_error { JS::js_undefined() };
};

}
