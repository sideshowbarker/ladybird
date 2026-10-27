/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Root.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Event.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(EventListenerList);

void EventListenerList::add(JS::FunctionObject& listener)
{
    if (has(listener))
        return;
    m_listeners.append(listener);
    if (m_listeners.size() == 1 && m_on_emptiness_change)
        m_on_emptiness_change->function()();
}

void EventListenerList::remove(JS::FunctionObject const& listener)
{
    if (m_listeners.remove_all_matching([&](auto const& candidate) { return candidate.ptr() == &listener; }) && m_listeners.is_empty() && m_on_emptiness_change)
        m_on_emptiness_change->function()();
}

bool EventListenerList::has(JS::FunctionObject const& listener) const
{
    return any_of(m_listeners, [&](auto const& candidate) { return candidate.ptr() == &listener; });
}

GC::RootVector<GC::Ref<JS::FunctionObject>> EventListenerList::listeners() const
{
    return { m_listeners.span() };
}

void EventListenerList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_listeners);
    visitor.visit(m_on_emptiness_change);
}

static JS::ThrowCompletionOr<GC::Ref<JS::FunctionObject>> listener_argument(JS::VM& vm)
{
    auto value = vm.argument(0);
    if (!value.is_function())
        return vm.throw_completion<JS::TypeError>("The listener must be a function"sv);
    return GC::Ref { value.as_function() };
}

GC::Ref<JS::Object> create_event(JS::Realm& realm, EventListenerList& listeners)
{
    auto event = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    // Each native function traces its captured list, so unreachable callback cycles can be collected.
    event->define_native_function(
        realm, "addListener"_utf16_fly_string, [listeners = GC::Ref { listeners }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!listeners->is_active())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            listeners->add(*TRY(listener_argument(vm)));
            return JS::js_undefined();
        },
        1, attributes);
    event->define_native_function(
        realm, "removeListener"_utf16_fly_string, [listeners = GC::Ref { listeners }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!listeners->is_active())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            listeners->remove(*TRY(listener_argument(vm)));
            return JS::js_undefined();
        },
        1, attributes);
    event->define_native_function(
        realm, "hasListener"_utf16_fly_string, [listeners = GC::Ref { listeners }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!listeners->is_active())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            return JS::Value { listeners->has(*TRY(listener_argument(vm))) };
        },
        1, attributes);
    return event;
}

}
