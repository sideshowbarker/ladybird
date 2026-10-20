/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Runtime.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

GC::Ref<JS::Object> create_runtime(JS::Realm& realm, ExtensionDescriptor const& descriptor, NamespaceContext, EventListenerList& on_message_listeners)
{
    auto runtime = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto extension_id = descriptor.id;
    auto manifest_json = descriptor.manifest_json;

    runtime->define_direct_property("id"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(extension_id)), attributes);

    // No API has failed in a way that sets lastError yet; it stays undefined until one does.
    runtime->define_direct_property("lastError"_utf16_fly_string, JS::js_undefined(), attributes);

    // getURL(path): the absolute URL of a resource of this extension.
    runtime->define_native_function(
        realm, "getURL"_utf16_fly_string, [extension_id](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto path = TRY(vm.argument(0).to_utf16_string(vm));
            StringBuilder url;
            url.append(scheme);
            url.append("://"sv);
            url.append(extension_id);
            url.append('/');
            auto utf8_path = path.to_utf8();
            url.append(utf8_path.bytes_as_string_view().trim("/"sv, TrimMode::Left));
            return JS::PrimitiveString::create(vm, Utf16String::from_utf8(url.string_view()));
        },
        1, attributes);

    // getManifest(): the manifest.json, parsed anew in this realm on every call.
    runtime->define_native_function(
        realm, "getManifest"_utf16_fly_string, [manifest_json](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            return JS::JSONObject::parse_json(vm, Utf16String::from_utf8(manifest_json));
        },
        0, attributes);

    runtime->define_direct_property("onMessage"_utf16_fly_string, create_event(realm, on_message_listeners), attributes);

    // sendMessage(message): routed between the extension's contexts by the UI process.
    runtime->define_native_function(
        realm, "sendMessage"_utf16_fly_string, [extension_id](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            (void)extension_id;
            auto& realm = *vm.current_realm();
            auto promise = WebIDL::create_promise(realm);
            WebIDL::reject_promise(realm, promise, JS::TypeError::create(realm, "runtime.sendMessage is not supported yet"_utf16));
            return promise->promise();
        },
        1, attributes);

    return runtime;
}

}
