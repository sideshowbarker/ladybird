/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Port.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Page/Page.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(Port);

Port::Port(MessagingContext& context, u64 port_id, u64 channel_id)
    : m_context(context)
    , m_realm(context.realm())
    , m_object(JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype()))
    , m_message_listeners(heap().allocate<EventListenerList>())
    , m_disconnect_listeners(heap().allocate<EventListenerList>())
    , m_port_id(port_id)
    , m_channel_id(channel_id)
{
}

GC::Ref<Port> Port::create(MessagingContext& context, u64 port_id, u64 channel_id, String const& name, GC::Ptr<JS::Object> sender)
{
    auto port = context.heap().allocate<Port>(context, port_id, channel_id);
    auto& realm = *port->m_realm;
    auto& object = port->object();
    auto attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable;
    object.define_direct_property("name"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(name)), attributes);
    if (sender)
        object.define_direct_property("sender"_utf16_fly_string, sender, attributes);
    object.define_native_accessor(realm, "error"_utf16_fly_string, [port](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> { return port->m_error; }, {}, attributes);
    object.define_direct_property("onMessage"_utf16_fly_string, create_event(realm, *port->m_message_listeners), attributes);
    object.define_direct_property("onDisconnect"_utf16_fly_string, create_event(realm, *port->m_disconnect_listeners), attributes);
    object.define_native_function(realm, "postMessage"_utf16_fly_string, [port](JS::VM& vm) { return port->post_message(vm.argument(0)); }, 1, attributes);
    object.define_native_function(realm, "disconnect"_utf16_fly_string, [port](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        port->disconnect();
        return JS::js_undefined(); }, 0, attributes);
    context.live_ports().set(port_id, port);
    return port;
}

void Port::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    visitor.visit(m_realm);
    visitor.visit(m_object);
    visitor.visit(m_message_listeners);
    visitor.visit(m_disconnect_listeners);
    visitor.visit(m_error);
}

JS::ThrowCompletionOr<JS::Value> Port::post_message(JS::Value message)
{
    auto& vm = m_realm->vm();
    if (m_disconnected || m_queue_overflow || m_context->is_detached())
        return vm.throw_completion<JS::Error>("Attempting to use a disconnected port object"sv);
    auto json = JS::JSONObject::stringify_impl(vm, message, JS::js_undefined(), JS::js_undefined());
    if (json.is_error() || !json.value().has_value())
        return vm.throw_completion<JS::TypeError>("The message can't be serialized as JSON"sv);
    if (m_disconnected || m_context->is_detached())
        return vm.throw_completion<JS::Error>("Attempting to use a disconnected port object"sv);
    if (!m_channel_id) {
        if (m_queued_messages.size() == 1024) {
            m_queue_overflow = true;
            m_queued_messages.clear();
        } else {
            m_queued_messages.append(json.value()->to_utf8());
        }
    } else {
        send(json.value()->to_utf8());
    }
    return JS::js_undefined();
}

void Port::send(String const& json)
{
    m_context->page().client().page_did_post_extension_port_message(m_context->context_id(), m_channel_id, m_port_id, json);
}

void Port::connected(u64 channel_id)
{
    if (m_disconnected || m_channel_id)
        return;
    m_channel_id = channel_id;
    if (m_queue_overflow) {
        m_context->page().client().page_did_disconnect_extension_port(m_context->context_id(), m_channel_id, m_port_id);
        receive_disconnect("Port message queue is full"_string);
        return;
    }
    auto queued = move(m_queued_messages);
    for (auto const& json : queued) {
        if (m_disconnected || m_context->is_detached())
            break;
        send(json);
    }
}

void Port::disconnect()
{
    if (m_disconnected)
        return;
    m_disconnected = true;
    m_queued_messages.clear();
    m_context->live_ports().remove(m_port_id);
    if (m_context->is_detached()) {
        m_message_listeners->revoke();
        m_disconnect_listeners->revoke();
    } else {
        m_message_listeners->clear();
        m_disconnect_listeners->clear();
    }
    m_context->page().client().page_did_disconnect_extension_port(m_context->context_id(), m_channel_id, m_port_id);
}

void Port::receive_message(String const& json)
{
    if (m_disconnected || m_queue_overflow || m_context->is_detached())
        return;
    HTML::TemporaryExecutionContext execution_context { *m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto message = JS::JSONObject::parse_json(m_realm->vm(), Utf16String::from_utf8(json));
    if (message.is_error())
        return;
    auto value = GC::make_root(message.release_value());
    auto listeners = m_message_listeners->listeners();
    for (auto listener : listeners) {
        if (m_disconnected || m_context->is_detached())
            break;
        auto result = JS::call(m_realm->vm(), *listener, JS::js_undefined(), value.value(), JS::Value { m_object });
        if (result.is_error())
            HTML::report_exception(result, *m_realm);
    }
}

void Port::receive_disconnect(Optional<String> const& error)
{
    if (m_disconnected || m_context->is_detached())
        return;
    m_disconnected = true;
    m_queued_messages.clear();
    m_context->live_ports().remove(m_port_id);
    m_message_listeners->clear();
    HTML::TemporaryExecutionContext execution_context { *m_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto previous = GC::make_root(m_context->last_error());
    ScopeGuard restore = [&] {
        if (error.has_value())
            m_context->set_last_error(previous.value());
        if (m_context->is_detached()) {
            m_message_listeners->revoke();
            m_disconnect_listeners->revoke();
        } else {
            m_disconnect_listeners->clear();
        }
    };
    if (error.has_value()) {
        auto message = Utf16String::from_utf8(*error);
        m_error = JS::Error::create(*m_realm, message);
        auto last_error = JS::Object::create(*m_realm, m_realm->intrinsics().object_prototype());
        last_error->define_direct_property("message"_utf16_fly_string, JS::PrimitiveString::create(m_realm->vm(), message), JS::Attribute::Enumerable);
        m_context->set_last_error(last_error);
    }
    auto listeners = m_disconnect_listeners->listeners();
    for (auto listener : listeners) {
        if (m_context->is_detached())
            break;
        auto result = JS::call(m_realm->vm(), *listener, JS::js_undefined(), JS::Value { m_object });
        if (result.is_error())
            HTML::report_exception(result, *m_realm);
    }
}

}
