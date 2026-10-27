/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Permissions.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_permissions_error(JS::VM& vm, StringView method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of permissions.{}(permissions.Permissions permissions, optional function callback): {}", method, detail));
}

// A permissions.Permissions object: optional arrays of strings named "permissions" and "origins".
static JS::ThrowCompletionOr<JsonObject> parse_permissions(JS::VM& vm, StringView method, JS::Value value)
{
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object())) {
        TRY(throw_permissions_error(vm, method, MUST(String::formatted("Error at parameter 'permissions': Invalid type: expected permissions.Permissions, found {}.", api_type_name(value)))));
        VERIFY_NOT_REACHED();
    }
    JsonObject parsed;
    for (auto key : { "permissions"sv, "origins"sv }) {
        auto list = TRY(value.as_object().get(Utf16FlyString::from_utf8(key)));
        if (list.is_undefined())
            continue;
        if (!list.is_object() || !is<JS::Array>(list.as_object()))
            TRY(throw_permissions_error(vm, method, MUST(String::formatted("Error at parameter 'permissions': Error at property '{}': Invalid type: expected array, found {}.", key, api_type_name(list)))));
        JsonArray strings;
        auto length = TRY(JS::length_of_array_like(vm, list.as_object()));
        for (size_t i = 0; i < length; ++i) {
            auto item = TRY(list.as_object().get(JS::PropertyKey { i }));
            if (!item.is_string())
                TRY(throw_permissions_error(vm, method, MUST(String::formatted("Error at parameter 'permissions': Error at property '{}': Error at index {}: Invalid type: expected string, found {}.", key, i, api_type_name(item)))));
            strings.must_append(item.as_string().utf16_string().to_utf8());
        }
        parsed.set(key, move(strings));
    }
    return parsed;
}

GC::Ref<JS::Object> create_permissions(JS::Realm& realm, MessagingContext& context)
{
    auto permissions = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    for (auto name : { "contains"sv, "request"sv, "remove"sv }) {
        permissions->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(1).is_function())
                callback = vm.argument(1).as_function();
            JsonArray arguments;
            arguments.must_append(TRY(parse_permissions(vm, name, vm.argument(0))));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("permissions.{}", name)), arguments)); }, 1, attributes);
    }
    permissions->define_native_function(realm, "getAll"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(0).is_function())
            callback = vm.argument(0).as_function();
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "permissions.getAll"sv, JsonArray {})); }, 0, attributes);
    permissions->define_direct_property("onAdded"_utf16_fly_string, create_event(realm, context.event_listeners("permissions.onAdded"_string)), attributes);
    permissions->define_direct_property("onRemoved"_utf16_fly_string, create_event(realm, context.event_listeners("permissions.onRemoved"_string)), attributes);
    return permissions;
}

}
