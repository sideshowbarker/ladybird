/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Runtime.h>
#include <LibWeb/Extensions/Namespace.h>

namespace Web::Extensions {

GC::Ref<JS::Object> install_namespace(JS::Realm& realm, ExtensionDescriptor const& descriptor, NamespaceContext context, EventListenerList& on_message_listeners)
{
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    auto namespace_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    namespace_object->define_direct_property("runtime"_utf16_fly_string, create_runtime(realm, descriptor, context, on_message_listeners), attributes);

    // Both names refer to the same object, so an extension written against either works.
    auto& global = realm.global_object();
    global.define_direct_property("browser"_utf16_fly_string, namespace_object, attributes);
    global.define_direct_property("chrome"_utf16_fly_string, namespace_object, attributes);
    return namespace_object;
}

}
