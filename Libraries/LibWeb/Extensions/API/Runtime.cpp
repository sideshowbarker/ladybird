/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Runtime.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

static JS::ThrowCompletionOr<bool> valid_message_options(JS::Value value)
{
    if (value.is_nullish())
        return true;
    if (!value.is_object())
        return false;
    auto keys = TRY(value.as_object().enumerable_own_property_names(JS::Object::PropertyKind::Key));
    for (auto key : keys) {
        if (!key.is_string() || key.as_string().utf16_string() != u"includeTlsChannelId"sv)
            return false;
        if (!TRY(value.as_object().get("includeTlsChannelId"_utf16_fly_string)).is_boolean())
            return false;
    }
    return true;
}

GC::Ref<JS::Object> create_runtime(JS::Realm& realm, MessagingContext& context, NamespaceContext namespace_context)
{
    auto const& descriptor = context.descriptor();
    auto runtime = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto extension_id = descriptor.id;
    auto manifest_json = descriptor.manifest_json;

    runtime->define_direct_property("id"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(extension_id)), attributes);

    runtime->define_native_accessor(realm, "lastError"_utf16_fly_string, [context = GC::Ref { context }](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> { return context->last_error(); }, {}, JS::Attribute::Configurable);

    // getURL(path): the absolute URL of a resource of this extension.
    runtime->define_native_function(
        realm, "getURL"_utf16_fly_string, [extension_id, context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
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
        realm, "getManifest"_utf16_fly_string, [manifest_json, context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            return JS::JSONObject::parse_json(vm, Utf16String::from_utf8(manifest_json));
        },
        0, attributes);

    // Only a background page is ever suspended, but every extension document has the events, as elsewhere.
    if (namespace_context == NamespaceContext::ExtensionDocument) {
        auto suspend_listeners = realm.heap().allocate<EventListenerList>();
        auto suspend_canceled_listeners = realm.heap().allocate<EventListenerList>();
        context.set_suspend_listeners(suspend_listeners, suspend_canceled_listeners);
        runtime->define_direct_property("onSuspend"_utf16_fly_string, create_event(realm, suspend_listeners), attributes);
        runtime->define_direct_property("onSuspendCanceled"_utf16_fly_string, create_event(realm, suspend_canceled_listeners), attributes);
    }

    auto connect_listeners = realm.heap().allocate<EventListenerList>();
    context.set_connect_listeners(connect_listeners);
    runtime->define_direct_property("onConnect"_utf16_fly_string, create_event(realm, connect_listeners), attributes);
    runtime->define_native_function(
        realm, "connect"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            if (vm.argument_count() > 2)
                return vm.throw_completion<JS::TypeError>("Invalid runtime.connect arguments"sv);
            Optional<String> extension_id;
            auto info = vm.argument(0);
            if (info.is_string()) {
                extension_id = info.as_string().utf16_string().to_utf8();
                info = vm.argument(1);
            } else if (vm.argument_count() > 1) {
                return vm.throw_completion<JS::TypeError>("The extension ID must be a string"sv);
            }
            String name;
            if (!info.is_nullish()) {
                if (!info.is_object())
                    return vm.throw_completion<JS::TypeError>("Invalid runtime.connect options"sv);
                auto value = TRY(info.as_object().get("name"_utf16_fly_string));
                if (!value.is_undefined()) {
                    if (!value.is_string())
                        return vm.throw_completion<JS::TypeError>("The port name must be a string"sv);
                    name = value.as_string().utf16_string().to_utf8();
                }
            }
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            return MessageRouter::the().connect(*context, move(extension_id), name);
        },
        0, attributes);

    runtime->define_direct_property("onMessage"_utf16_fly_string, create_event(realm, context.message_listeners()), attributes);

    runtime->define_native_function(
        realm, "sendMessage"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            if (!count || count > 3)
                return vm.throw_completion<JS::TypeError>("Invalid runtime.sendMessage arguments"sv);

            auto extension_id = JS::js_undefined();
            auto message = vm.argument(0);
            if (count == 3 || (count == 2 && !TRY(valid_message_options(vm.argument(1))))) {
                extension_id = vm.argument(0);
                message = vm.argument(1);
                if (!extension_id.is_nullish() && !extension_id.is_string())
                    return vm.throw_completion<JS::TypeError>("The extension ID must be a string"sv);
                if (count == 3 && !TRY(valid_message_options(vm.argument(2))))
                    return vm.throw_completion<JS::TypeError>("Invalid runtime.sendMessage options"sv);
            }

            auto& realm = context->realm();
            auto promise = extension_id.is_string() && !extension_id.as_string().utf16_string().is_empty() && extension_id.as_string().utf16_string().to_utf8() != context->extension_id()
                ? WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Cross-extension messaging isn't supported"_utf16))
                : MessageRouter::the().send(*context, message);
            return settle_api_call(*context, callback, promise);
        },
        1, attributes);

    return runtime;
}

}
