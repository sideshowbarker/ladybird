/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Storage.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

JS::Value storage_value_from_json(JS::Realm& realm, JsonValue const& value)
{
    if (value.is_object()) {
        auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
        value.as_object().for_each_member([&](auto const& key, auto const& item) {
            object->define_direct_property(Utf16String::from_utf8(key), storage_value_from_json(realm, item), JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);
        });
        return object;
    }
    if (value.is_array()) {
        GC::RootVector<JS::Value> values;
        for (auto const& item : value.as_array().values())
            values.append(storage_value_from_json(realm, item));
        return JS::Array::create_from(realm, values);
    }
    if (value.is_string())
        return JS::PrimitiveString::create(realm.vm(), Utf16String::from_utf8(value.as_string()));
    if (value.is_bool())
        return JS::Value(value.as_bool());
    if (value.is_number())
        return JS::Value(value.get_double_with_precision_loss().value());
    return JS::js_null();
}

static JS::ThrowCompletionOr<JsonObject> serialize_items(JS::VM& vm, JS::Value value)
{
    if (!value.is_object() || TRY(value.is_array(vm)))
        return vm.throw_completion<JS::TypeError>("Storage items must be an object"sv);
    auto json = TRY(JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::js_undefined()));
    if (!json.has_value())
        return vm.throw_completion<JS::TypeError>("Storage items cannot be serialized as JSON"sv);
    auto parsed = JsonValue::from_string(json->to_utf8());
    if (parsed.is_error() || !parsed.value().is_object())
        return vm.throw_completion<JS::TypeError>("Storage items must serialize to an object"sv);
    return parsed.release_value().as_object();
}

struct PreparedStorageRequest {
    StorageRequest request;
    Optional<JsonObject> defaults;
};

static JS::ThrowCompletionOr<PreparedStorageRequest> prepare_request(JS::VM& vm, StorageRequest::Operation operation, size_t count)
{
    using Operation = StorageRequest::Operation;
    PreparedStorageRequest prepared { .request = { .operation = operation, .keys = {}, .items_json = {} }, .defaults = {} };
    if (count > (operation == Operation::Clear ? 0u : 1u) || (!count && (operation == Operation::Set || operation == Operation::Remove)))
        return vm.throw_completion<JS::TypeError>("Invalid storage.local arguments"sv);
    if (operation == Operation::Clear)
        return prepared;
    auto value = count ? vm.argument(0) : JS::js_undefined();
    if (operation == Operation::Set) {
        prepared.request.items_json = TRY(serialize_items(vm, value)).serialized();
        return prepared;
    }
    if (value.is_nullish() && operation != Operation::Remove)
        return prepared;
    Vector<String> keys;
    if (value.is_string()) {
        keys.append(value.as_string().utf16_string().to_utf8());
    } else if (TRY(value.is_array(vm))) {
        auto length = TRY(JS::length_of_array_like(vm, value.as_object()));
        for (size_t index = 0; index < length; ++index) {
            auto key = TRY(value.as_object().get(index));
            if (!key.is_string())
                return vm.throw_completion<JS::TypeError>("Storage keys must be strings"sv);
            keys.append(key.as_string().utf16_string().to_utf8());
        }
    } else if (value.is_object() && operation == Operation::Get) {
        prepared.defaults = TRY(serialize_items(vm, value));
        prepared.defaults->for_each_member([&](auto const& key, auto const&) { keys.append(key); });
    } else {
        return vm.throw_completion<JS::TypeError>("Invalid storage keys"sv);
    }
    prepared.request.keys = move(keys);
    return prepared;
}

GC::Ref<JS::Object> create_storage(JS::Realm& realm, MessagingContext& context)
{
    auto storage = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto local = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto global_listeners = realm.heap().allocate<EventListenerList>();
    auto local_listeners = realm.heap().allocate<EventListenerList>();
    context.set_storage_listeners(global_listeners, local_listeners);
    storage->define_direct_property("onChanged"_utf16_fly_string, create_event(realm, global_listeners), attributes);
    local->define_direct_property("onChanged"_utf16_fly_string, create_event(realm, local_listeners), attributes);
    storage->define_direct_property("local"_utf16_fly_string, local, attributes);
    local->define_direct_property("QUOTA_BYTES"_utf16_fly_string, JS::Value(10485760), 0);
    struct Method {
        Utf16FlyString name;
        StorageRequest::Operation operation;
    };
    for (auto const& method : { Method { "get"_utf16_fly_string, StorageRequest::Operation::Get }, Method { "set"_utf16_fly_string, StorageRequest::Operation::Set }, Method { "remove"_utf16_fly_string, StorageRequest::Operation::Remove }, Method { "clear"_utf16_fly_string, StorageRequest::Operation::Clear }, Method { "getBytesInUse"_utf16_fly_string, StorageRequest::Operation::BytesInUse } }) {
        local->define_native_function(realm, method.name, [context = GC::Ref { context }, operation = method.operation](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            auto& operation_realm = context->realm();
            auto prepared = prepare_request(vm, operation, count);
            if (context->is_detached())
                return settle_api_call(*context, callback, WebIDL::create_rejected_promise(operation_realm, JS::TypeError::create(operation_realm, "The extension context is detached"_utf16)));
            auto promise = prepared.is_error()
                ? WebIDL::create_rejected_promise(operation_realm, prepared.release_error().value())
                : MessageRouter::the().request_storage(*context, prepared.value().request, move(prepared.value().defaults));
            return settle_api_call(*context, callback, promise); }, method.operation == StorageRequest::Operation::Clear ? 0 : 1, attributes);
    }
    return storage;
}

}
