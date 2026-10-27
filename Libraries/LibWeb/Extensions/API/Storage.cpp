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

static JS::ThrowCompletionOr<PreparedStorageRequest> prepare_request(JS::VM& vm, StorageRequest::Area area, StorageRequest::Operation operation, size_t count)
{
    using Operation = StorageRequest::Operation;
    PreparedStorageRequest prepared { .request = { .operation = operation, .keys = {}, .items_json = {}, .area = area }, .defaults = {} };
    if (count > (operation == Operation::Clear ? 0u : 1u) || (!count && (operation == Operation::Set || operation == Operation::Remove || operation == Operation::SetAccessLevel)))
        return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Invalid storage.{} arguments", storage_area_name(area)));
    if (operation == Operation::Clear)
        return prepared;
    auto value = count ? vm.argument(0) : JS::js_undefined();
    // setAccessLevel({ accessLevel }) sends the level alone.
    if (operation == Operation::SetAccessLevel) {
        if (!value.is_object())
            return vm.throw_completion<JS::TypeError>("Invalid storage.session arguments"sv);
        auto level = TRY(value.as_object().get("accessLevel"_utf16_fly_string));
        if (!level.is_string())
            return vm.throw_completion<JS::TypeError>("Invalid storage.session arguments"sv);
        prepared.request.items_json = level.as_string().utf16_string().to_utf8();
        return prepared;
    }
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

// One storage area's object, storage.local or storage.session, with its methods and its own onChanged.
static GC::Ref<JS::Object> create_storage_area(JS::Realm& realm, MessagingContext& context, StorageRequest::Area area, EventListenerList& listeners)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    object->define_direct_property("onChanged"_utf16_fly_string, create_event(realm, listeners), attributes);
    if (area == StorageRequest::Area::Sync) {
        object->define_direct_property("QUOTA_BYTES"_utf16_fly_string, JS::Value(102400), 0);
        object->define_direct_property("QUOTA_BYTES_PER_ITEM"_utf16_fly_string, JS::Value(8192), 0);
        object->define_direct_property("MAX_ITEMS"_utf16_fly_string, JS::Value(512), 0);
        // Exposed, as extensions read them, but not enforced: nothing syncs, so writing often costs nothing.
        object->define_direct_property("MAX_WRITE_OPERATIONS_PER_HOUR"_utf16_fly_string, JS::Value(1800), 0);
        object->define_direct_property("MAX_WRITE_OPERATIONS_PER_MINUTE"_utf16_fly_string, JS::Value(120), 0);
    } else if (area != StorageRequest::Area::Managed) {
        object->define_direct_property("QUOTA_BYTES"_utf16_fly_string, JS::Value(10485760), 0);
    }
    struct Method {
        Utf16FlyString name;
        StorageRequest::Operation operation;
    };
    Vector<Method> methods { { "get"_utf16_fly_string, StorageRequest::Operation::Get }, { "set"_utf16_fly_string, StorageRequest::Operation::Set }, { "remove"_utf16_fly_string, StorageRequest::Operation::Remove }, { "clear"_utf16_fly_string, StorageRequest::Operation::Clear }, { "getBytesInUse"_utf16_fly_string, StorageRequest::Operation::BytesInUse } };
    if (area == StorageRequest::Area::Session)
        methods.append({ "setAccessLevel"_utf16_fly_string, StorageRequest::Operation::SetAccessLevel });
    for (auto const& method : methods) {
        object->define_native_function(realm, method.name, [context = GC::Ref { context }, area, operation = method.operation](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            auto& operation_realm = context->realm();
            auto prepared = prepare_request(vm, area, operation, count);
            if (context->is_detached())
                return settle_api_call(*context, callback, WebIDL::create_rejected_promise(operation_realm, JS::TypeError::create(operation_realm, "The extension context is detached"_utf16)));
            auto promise = prepared.is_error()
                ? WebIDL::create_rejected_promise(operation_realm, prepared.release_error().value())
                : MessageRouter::the().request_storage(*context, prepared.value().request, move(prepared.value().defaults));
            return settle_api_call(*context, callback, promise); }, method.operation == StorageRequest::Operation::Clear ? 0 : 1, attributes);
    }
    return object;
}

GC::Ref<JS::Object> create_storage(JS::Realm& realm, MessagingContext& context)
{
    auto storage = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto global_listeners = realm.heap().allocate<EventListenerList>();
    context.set_storage_listeners(global_listeners);
    storage->define_direct_property("onChanged"_utf16_fly_string, create_event(realm, global_listeners), attributes);
    // A content script has storage.session too, but the UI process refuses its calls until the extension allows them.
    for (auto area : { StorageRequest::Area::Local, StorageRequest::Area::Session, StorageRequest::Area::Sync, StorageRequest::Area::Managed }) {
        auto name = MUST(String::from_utf8(storage_area_name(area)));
        auto listeners = realm.heap().allocate<EventListenerList>();
        context.set_storage_area_listeners(name, listeners);
        storage->define_direct_property(Utf16FlyString::from_utf8(name), create_storage_area(realm, context, area, listeners), attributes);
    }
    return storage;
}

}
