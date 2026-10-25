/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

// Collects the reply to one dispatched message across every listener it was delivered to: the first listener to
// answer, by calling sendResponse or by resolving a promise it returned, decides the reply; once every listener
// has declined (returned without keeping the channel open, or had its promise reject), the reply is empty.
class ReplyCollector final : public GC::Cell {
    GC_CELL(ReplyCollector, GC::Cell);
    GC_DECLARE_ALLOCATOR(ReplyCollector);

public:
    void cancel()
    {
        if (m_done)
            return;
        m_done = true;
        m_page->client().page_did_reply_to_extension_runtime_message(m_dispatch_id, {});
        MessageRouter::the().finished_dispatch(m_dispatch_id);
    }

    void expect_answer() { ++m_outstanding; }

    void reply(JS::Realm& realm, JS::Value value)
    {
        if (m_done)
            return;
        m_done = true;
        auto json = JS::JSONObject::stringify_impl(realm.vm(), value, JS::js_undefined(), JS::js_undefined());
        Optional<String> reply_json;
        // A reply that can't be serialized counts as none.
        if (!json.is_error() && json.value().has_value())
            reply_json = json.value()->to_utf8();
        m_page->client().page_did_reply_to_extension_runtime_message(m_dispatch_id, reply_json.has_value() ? MessageResult { MessageResult::Reply { *reply_json } } : MessageResult {});
        MessageRouter::the().finished_dispatch(m_dispatch_id);
    }

    void decline()
    {
        if (m_done)
            return;
        VERIFY(m_outstanding > 0);
        if (--m_outstanding > 0)
            return;
        cancel();
    }

private:
    ReplyCollector(Page& page, u64 dispatch_id)
        : m_page(page)
        , m_dispatch_id(dispatch_id)
    {
    }

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ref<Page> m_page;
    u64 m_dispatch_id { 0 };
    size_t m_outstanding { 0 };
    bool m_done { false };
};

GC_DEFINE_ALLOCATOR(ReplyCollector);

void MessageRouter::finished_dispatch(u64 dispatch_id)
{
    m_pending_dispatches.remove(dispatch_id);
}

MessageRouter::MessageRouter()
{
    ExtensionSet::the().on_changed = [this] { reconcile_contexts(); };
}

void MessageRouter::reconcile_contexts()
{
    GC::RootVector<GC::Ref<MessagingContext>> contexts;
    for (auto context : m_contexts)
        contexts.append(*context);
    for (auto context : contexts) {
        auto const* descriptor = ExtensionSet::the().find(context->extension_id());
        if (!descriptor || *descriptor != context->descriptor())
            context->detach();
    }
}

MessageRouter& MessageRouter::the()
{
    static MessageRouter* router = new MessageRouter;
    return *router;
}

void MessageRouter::register_context(MessagingContext& context)
{
    m_contexts.set(&context);
    context.page().client().page_did_create_extension_context(context.extension_id(), context.context_id(), context.document().id(), context.descriptor().revision);
}

void MessageRouter::unregister_context(MessagingContext& context)
{
    if (!m_contexts.remove(&context))
        return;
    m_pending_sends.remove_all_matching([&](auto, auto const& pending) { return pending.context.ptr() == &context; });
    Vector<u64> dispatches;
    for (auto const& [id, pending] : m_pending_dispatches) {
        if (pending.context.ptr() == &context)
            dispatches.append(id);
    }
    for (auto id : dispatches) {
        if (auto pending = m_pending_dispatches.take(id); pending.has_value())
            pending->collector->cancel();
    }
    context.page().client().page_did_destroy_extension_context(context.extension_id(), context.context_id());
}

GC::Ref<JS::PromiseCapability> MessageRouter::send(MessagingContext& context, JS::Value message)
{
    auto& realm = context.realm();
    auto& vm = realm.vm();

    // A message that can't be serialized rejects the promise rather than throwing.
    auto json = JS::JSONObject::stringify_impl(vm, message, JS::js_undefined(), JS::js_undefined());
    if (json.is_error())
        return WebIDL::create_rejected_promise(realm, json.release_error().value());
    if (!json.value().has_value())
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "The message can't be serialized as JSON"_utf16));

    if (context.is_detached())
        return WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "The extension context is detached"_utf16));

    auto promise = WebIDL::create_promise(realm);
    auto request_id = m_next_request_id++;
    m_pending_sends.set(request_id, PendingSend { GC::make_root(promise), GC::make_root(realm), &context.page(), &context });
    context.page().client().page_did_send_extension_runtime_message(context.extension_id(), request_id, context.sender(), json.value()->to_utf8(), context.context_id());
    return promise;
}

void MessageRouter::complete(Page& page, u64 request_id, Web::Extensions::MessageResult const& reply_json)
{
    auto candidate = m_pending_sends.get(request_id);
    if (!candidate.has_value() || candidate->page.ptr() != &page)
        return;
    auto pending = m_pending_sends.take(request_id);

    auto& realm = *pending->realm;
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    auto value = JS::js_undefined();
    if (reply_json.value.has<MessageResult::Error>()) {
        WebIDL::reject_promise(realm, *pending->promise, JS::Error::create(realm, Utf16String::from_utf8(reply_json.value.get<MessageResult::Error>().message)));
        return;
    }
    if (reply_json.value.has<MessageResult::Reply>()) {
        auto parsed = JS::JSONObject::parse_json(realm.vm(), Utf16String::from_utf8(reply_json.value.get<MessageResult::Reply>().json));
        if (!parsed.is_error())
            value = parsed.value();
    }
    WebIDL::resolve_promise(realm, *pending->promise, value);
}

static GC::Ref<JS::Object> sender_object(JS::Realm& realm, MessageSender const& sender)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable;
    object->define_direct_property("id"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(sender.extension_id)), attributes);
    if (sender.url.has_value())
        object->define_direct_property("url"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(*sender.url)), attributes);
    return object;
}

void MessageRouter::dispatch(Page& page, String const& extension_id, u64 dispatch_id, MessageSender const& sender, String const& message_json, u64 context_id)
{
    struct Target {
        GC::Root<MessagingContext> context;
        GC::RootVector<GC::Ref<JS::FunctionObject>> listeners;
    };
    Vector<Target> targets;
    for (auto context : m_contexts) {
        if (!context->is_detached() && &context->page() == &page && context->extension_id() == extension_id && context->context_id() == context_id)
            targets.append({ GC::make_root(*context), context->message_listeners().listeners() });
    }

    auto collector = page.heap().allocate<ReplyCollector>(page, dispatch_id);
    // Every listener is counted before any runs, so an early reply can't finish the count under a later one.
    size_t listener_count = 0;
    for (auto const& target : targets)
        listener_count += target.listeners.size();
    if (listener_count == 0) {
        page.client().page_did_reply_to_extension_runtime_message(dispatch_id, MessageResult::NoListener {});
        return;
    }
    m_pending_dispatches.set(dispatch_id, PendingDispatch { GC::make_root(collector), targets[0].context.ptr() });
    for (size_t i = 0; i < listener_count; ++i)
        collector->expect_answer();

    for (auto const& target : targets) {
        auto const& context = target.context;
        if (context->is_detached()) {
            for (size_t i = 0; i < target.listeners.size(); ++i)
                collector->decline();
            continue;
        }
        auto& realm = context->realm();
        auto& vm = realm.vm();
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto message = JS::JSONObject::parse_json(vm, Utf16String::from_utf8(message_json));
        if (message.is_error()) {
            for (size_t i = 0; i < target.listeners.size(); ++i)
                collector->decline();
            continue;
        }
        auto sender_value = sender_object(realm, sender);

        for (auto const& listener : target.listeners) {
            if (context->is_detached()) {
                collector->decline();
                continue;
            }
            // sendResponse answers once; a later call, or one after the context is gone, does nothing.
            auto send_response = JS::NativeFunction::create(
                realm, "sendResponse"_utf16_fly_string, [collector, context = GC::Ref { *context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                    if (!context->is_detached())
                        collector->reply(context->realm(), vm.argument(0));
                    return JS::js_undefined();
                });

            Array<JS::Value, 3> arguments { message.value(), JS::Value { sender_value }, JS::Value { send_response } };
            auto result = JS::call(vm, JS::Value { listener }, JS::js_undefined(), ReadonlySpan<JS::Value> { arguments });
            if (result.is_error()) {
                collector->decline();
                continue;
            }
            auto value = result.value();

            // Returning true keeps the channel open for a later sendResponse.
            if (value.is_boolean() && value.as_bool())
                continue;

            // A returned promise answers with what it resolves to, or declines when it rejects.
            if (value.is_object() && is<JS::Promise>(value.as_object())) {
                auto promise = WebIDL::create_resolved_promise(realm, value);
                WebIDL::react_to_promise(
                    promise,
                    GC::create_function(vm.heap(), [collector, context = GC::Ref { *context }](JS::Value resolved) -> WebIDL::ExceptionOr<JS::Value> {
                        if (!context->is_detached())
                            collector->reply(context->realm(), resolved);
                        else
                            collector->decline();
                        return JS::js_undefined();
                    }),
                    GC::create_function(vm.heap(), [collector](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                        collector->decline();
                        return JS::js_undefined();
                    }));
                continue;
            }

            // Anything else ends this listener's part without an answer.
            collector->decline();
        }
    }
}

}
