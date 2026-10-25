/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WorldSafeValue.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::Bindings {

JS::Value world_safe_value(JS::Realm& realm, JS::Value value)
{
    if (!value.is_object())
        return value;
    if (&host_defined_wrapper_world(value.as_object().shape().realm()) == &host_defined_wrapper_world(realm))
        return value;

    // AD-HOC: The DOM getters return the stored value. Across isolated worlds, return a clone so prototypes and
    // callable properties can't expose the source global object.
    auto serialized = HTML::structured_serialize(realm.vm(), value);
    if (serialized.is_error())
        return JS::js_null();
    auto cloned = HTML::structured_deserialize(realm.vm(), serialized.release_value(), realm);
    if (cloned.is_error())
        return JS::js_null();
    return cloned.release_value();
}

JS::Value cached_world_safe_value(JS::Realm& realm, JS::Value value)
{
    if (!value.is_object())
        return value;
    auto& world = host_defined_wrapper_world(realm);
    if (&host_defined_wrapper_world(value.as_object().shape().realm()) == &world)
        return value;

    static NeverDestroyed<WrapperWorldWeakValueCacheMap<JS::Object, JS::Object>> caches;
    auto source = GC::make_root(value.as_object());
    if (auto cached = caches->cache_for(*source).get(world))
        return JS::Value { cached };
    auto cloned = world_safe_value(realm, source.ptr());
    if (cloned.is_object())
        caches->cache_for(*source).set(world, cloned.as_object());
    return cloned;
}

Variant<GC::Ref<DOM::Event>, Empty> world_safe_event(JS::Realm& realm, Variant<GC::Ref<DOM::Event>, Empty> event)
{
    // AD-HOC: The Window.event getter returns the current event. A shared Window's current error event isn't visible
    // outside the world reporting that error.
    if (event.has<GC::Ref<DOM::Event>>()) {
        auto const* error = as_if<HTML::ErrorEvent>(event.get<GC::Ref<DOM::Event>>().ptr());
        if (error && !error->can_dispatch_in_world(host_defined_wrapper_world(realm)))
            return Empty {};
    }
    return event;
}

}
