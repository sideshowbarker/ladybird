/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Runtime.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
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

GC::Ref<JS::Object> create_runtime(JS::Realm& realm, MessagingContext& context, NamespaceContext)
{
    auto const& descriptor = context.descriptor();
    auto runtime = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto extension_id = descriptor.id;
    auto manifest_json = descriptor.manifest_json;

    runtime->define_direct_property("id"_utf16_fly_string, JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(extension_id)), attributes);

    auto error_state = JS::Object::create(realm, nullptr);
    error_state->define_direct_property("value"_utf16_fly_string, JS::js_undefined(), attributes);
    runtime->define_native_accessor(realm, "lastError"_utf16_fly_string, [error_state](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> { return error_state->get("value"_utf16_fly_string); }, {}, JS::Attribute::Configurable);

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

    runtime->define_direct_property("onMessage"_utf16_fly_string, create_event(realm, context.message_listeners()), attributes);

    runtime->define_native_function(
        realm, "sendMessage"_utf16_fly_string, [context = GC::Ref { context }, error_state](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::Object> callback;
            if (count && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_object();
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
            JS::Value result;
            if (extension_id.is_string() && !extension_id.as_string().utf16_string().is_empty() && extension_id.as_string().utf16_string().to_utf8() != context->extension_id())
                result = WebIDL::create_rejected_promise(realm, JS::TypeError::create(realm, "Cross-extension messaging isn't supported"_utf16))->promise();
            else
                result = TRY(MessageRouter::the().send(*context, message));
            if (!callback)
                return result;

            auto invoke = [context, callback = GC::Ref { *callback }, error_state](JS::Value value, bool failed) -> WebIDL::ExceptionOr<JS::Value> {
                if (context->is_detached())
                    return JS::js_undefined();
                auto& realm = context->realm();
                if (failed) {
                    auto error = JS::Object::create(realm, realm.intrinsics().object_prototype());
                    auto message = value;
                    if (value.is_object()) {
                        auto property = value.as_object().get("message"_utf16_fly_string);
                        if (!property.is_error())
                            message = property.value();
                    }
                    auto text = message.to_utf16_string(realm.vm());
                    error->define_direct_property("message"_utf16_fly_string,
                        JS::PrimitiveString::create(realm.vm(), text.is_error() ? "The message failed"_utf16 : text.release_value()), JS::Attribute::Enumerable);
                    value = error;
                }
                auto previous = GC::make_root(MUST(error_state->get("value"_utf16_fly_string)));
                error_state->define_direct_property("value"_utf16_fly_string, failed ? value : JS::js_undefined(), JS::Attribute::Writable);
                ScopeGuard restore = [&] { error_state->define_direct_property("value"_utf16_fly_string, previous.value(), JS::Attribute::Writable); };
                auto completion = JS::call(realm.vm(), callback, JS::js_undefined(), failed ? JS::js_undefined() : value);
                if (completion.is_error())
                    HTML::report_exception(completion, realm);
                return JS::js_undefined();
            };
            WebIDL::react_to_promise(WebIDL::create_resolved_promise(realm, result),
                GC::create_function(vm.heap(), [invoke](JS::Value value) mutable { return invoke(value, false); }),
                GC::create_function(vm.heap(), [invoke](JS::Value value) mutable { return invoke(value, true); }));
            return JS::js_undefined();
        },
        1, attributes);

    return runtime;
}

}
