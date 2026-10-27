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
#include <LibURL/Parser.h>
#include <LibWeb/Extensions/API/Arguments.h>
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
        runtime->define_direct_property("onInstalled"_utf16_fly_string, create_event(realm, context.event_listeners("runtime.onInstalled"_string)), attributes);
        runtime->define_direct_property("onStartup"_utf16_fly_string, create_event(realm, context.event_listeners("runtime.onStartup"_string)), attributes);
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

    // openOptionsPage(): The UI process opens the options page in a tab, or activates a tab showing it already.
    if (namespace_context == NamespaceContext::ExtensionDocument) {
        runtime->define_native_function(
            realm, "openOptionsPage"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (context->is_detached())
                    return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
                GC::Ptr<JS::FunctionObject> callback;
                if (vm.argument(0).is_function())
                    callback = vm.argument(0).as_function();
                return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "runtime.openOptionsPage"sv, {}));
            },
            0, attributes);

        // setUninstallURL(url): Nothing visits it, since Ladybird has no uninstall page to send the user to; an http or
        // https URL (or none) is accepted, and anything else fails as extensions expect.
        runtime->define_native_function(
            realm, "setUninstallURL"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                if (context->is_detached())
                    return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
                GC::Ptr<JS::FunctionObject> callback;
                if (vm.argument(1).is_function())
                    callback = vm.argument(1).as_function();
                if (!vm.argument(0).is_string())
                    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of runtime.setUninstallURL(string url, optional function callback): Error at parameter 'url': Invalid type: expected string, found {}.", api_type_name(vm.argument(0))));
                auto url = vm.argument(0).as_string().utf16_string().to_utf8();
                auto& realm = context->realm();
                auto parsed = URL::Parser::basic_parse(url);
                if (!url.is_empty() && (!parsed.has_value() || (parsed->scheme() != "http"sv && parsed->scheme() != "https"sv))) {
                    auto message = MUST(String::formatted("Invalid URL: \"{}\".", url));
                    return settle_api_call(*context, callback, WebIDL::create_rejected_promise(realm, JS::Error::create(realm, Utf16String::from_utf8(message))));
                }
                return settle_api_call(*context, callback, WebIDL::create_resolved_promise(realm, JS::js_undefined()));
            },
            1, attributes);
    }

    // getPlatformInfo(): The values are the ones the API defines for this platform and processor.
    runtime->define_native_function(
        realm, "getPlatformInfo"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(0).is_function())
                callback = vm.argument(0).as_function();
#if defined(AK_OS_MACOS)
            auto os = "mac"sv;
#elif defined(AK_OS_WINDOWS)
            auto os = "win"sv;
#elif defined(AK_OS_ANDROID)
            auto os = "android"sv;
#elif defined(AK_OS_OPENBSD)
            auto os = "openbsd"sv;
#else
            auto os = "linux"sv;
#endif
#if ARCH(AARCH64)
            auto arch = "arm64"sv;
#elif ARCH(X86_64)
            auto arch = "x86-64"sv;
#elif ARCH(RISCV64)
            auto arch = "riscv64"sv;
#else
            auto arch = "x86-32"sv;
#endif
            auto& realm = context->realm();
            auto info = JS::Object::create(realm, realm.intrinsics().object_prototype());
            info->define_direct_property("os"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(os)), JS::default_attributes);
            info->define_direct_property("arch"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(arch)), JS::default_attributes);
            info->define_direct_property("nacl_arch"_utf16_fly_string, JS::PrimitiveString::create(vm, Utf16String::from_utf8(arch)), JS::default_attributes);
            return settle_api_call(*context, callback, WebIDL::create_resolved_promise(realm, info));
        },
        0, attributes);

    return runtime;
}

}
