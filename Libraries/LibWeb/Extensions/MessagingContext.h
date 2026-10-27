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
#include <LibWeb/Extensions/API/WebRequest.h>
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

    // storage.onChanged's listeners, and each area's own onChanged listeners by its name ("local", e.g.).
    void set_storage_listeners(GC::Ref<EventListenerList> global) { m_storage_listeners = global; }
    void set_storage_area_listeners(String area_name, GC::Ref<EventListenerList> listeners) { m_storage_area_listeners.set(move(area_name), listeners); }
    GC::Ptr<EventListenerList> storage_listeners() { return m_storage_listeners; }
    GC::Ptr<EventListenerList> storage_area_listeners(StringView area_name)
    {
        auto listeners = m_storage_area_listeners.get(area_name);
        return listeners.has_value() ? listeners->ptr() : nullptr;
    }
    void revoke_storage_listeners()
    {
        if (m_storage_listeners)
            m_storage_listeners->revoke();
        for (auto& [name, listeners] : m_storage_area_listeners)
            listeners->revoke();
    }

    void set_connect_listeners(GC::Ref<EventListenerList> listeners) { m_connect_listeners = listeners; }
    GC::Ptr<EventListenerList> connect_listeners() { return m_connect_listeners; }
    void set_suspend_listeners(GC::Ref<EventListenerList> suspend, GC::Ref<EventListenerList> suspend_canceled)
    {
        m_suspend_listeners = suspend;
        m_suspend_canceled_listeners = suspend_canceled;
    }
    GC::Ptr<EventListenerList> suspend_listeners(bool canceled) { return canceled ? m_suspend_canceled_listeners : m_suspend_listeners; }
    HashMap<u64, GC::Ref<Port>>& live_ports() { return m_live_ports; }

    void set_web_request_listeners(GC::Ref<WebRequestListeners> listeners) { m_web_request_listeners = listeners; }
    GC::Ptr<WebRequestListeners> web_request_listeners() { return m_web_request_listeners; }

    u64 context_id() const { return m_context_id; }
    virtual DOM::Document& document() = 0;

    virtual JS::Realm& realm() = 0;
    virtual ExtensionDescriptor const& descriptor() const = 0;
    // A grant changes what the extension may do without reloading it, so its contexts take the new descriptor.
    virtual void set_descriptor(ExtensionDescriptor) = 0;
    virtual EventListenerList& message_listeners() = 0;
    virtual Page& page() = 0;
    virtual MessageSender sender() const = 0;
    virtual bool is_detached() const = 0;
    virtual void detach() = 0;

    String const& extension_id() const { return descriptor().id; }

    // The listeners of an API event by its name ("action.onClicked", e.g.), made when first asked for.
    EventListenerList& event_listeners(String const& event_name);
    GC::Ptr<EventListenerList> existing_event_listeners(String const& event_name) const;
    // An extension document's context tells the UI process which of these events it listens to, so that a stopped
    // background page wakes only for those.
    void report_event_listeners();

protected:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_connect_listeners);
        visitor.visit(m_suspend_listeners);
        visitor.visit(m_suspend_canceled_listeners);
        for (auto const& [id, port] : m_live_ports)
            visitor.visit(port);
        visitor.visit(m_last_error);
        visitor.visit(m_storage_listeners);
        visitor.visit(m_web_request_listeners);
        for (auto const& [name, listeners] : m_storage_area_listeners)
            visitor.visit(listeners);
        for (auto const& [name, listeners] : m_event_listeners)
            visitor.visit(listeners);
    }

    MessagingContext()
        : m_context_id(++s_next_context_id)
    {
    }

private:
    inline static u64 s_next_context_id { 0 };
    u64 m_context_id;
    GC::Ptr<EventListenerList> m_connect_listeners;
    GC::Ptr<EventListenerList> m_suspend_listeners;
    GC::Ptr<EventListenerList> m_suspend_canceled_listeners;
    HashMap<u64, GC::Ref<Port>> m_live_ports;
    GC::Ptr<EventListenerList> m_storage_listeners;
    GC::Ptr<WebRequestListeners> m_web_request_listeners;
    HashMap<String, GC::Ref<EventListenerList>> m_storage_area_listeners;
    HashMap<String, GC::Ref<EventListenerList>> m_event_listeners;
    JS::Value m_last_error { JS::js_undefined() };
};

}
