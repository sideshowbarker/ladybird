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
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Alarms.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// One alarms method's name and the signature its errors quote.
struct AlarmMethod {
    StringView name;
    StringView signature;
};

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_alarm_error(JS::VM& vm, AlarmMethod const& method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of alarms.{}({}): {}", method.name, method.signature, detail));
}

static JS::ThrowCompletionOr<JsonObject> parse_alarm_info(JS::VM& vm, AlarmMethod const& method, JS::Value value)
{
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object())) {
        TRY(throw_alarm_error(vm, method, MUST(String::formatted("Error at parameter 'alarmInfo': Invalid type: expected object, found {}.", api_type_name(value)))));
        VERIFY_NOT_REACHED();
    }
    JsonObject info;
    auto& object = value.as_object();
    for (auto const& property_key : TRY(object.internal_own_property_keys())) {
        auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
        if (key.is_symbol())
            continue;
        auto descriptor = TRY(object.internal_get_own_property(key));
        if (!descriptor.has_value() || !*descriptor->enumerable)
            continue;
        auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
        if (name != "when"sv && name != "delayInMinutes"sv && name != "periodInMinutes"sv)
            TRY(throw_alarm_error(vm, method, MUST(String::formatted("Error at parameter 'alarmInfo': Unexpected property: '{}'.", name))));
        auto property = TRY(object.get(key));
        if (property.is_undefined())
            continue;
        if (!property.is_number())
            TRY(throw_alarm_error(vm, method, MUST(String::formatted("Error at parameter 'alarmInfo': Error at property '{}': Invalid type: expected number, found {}.", name, api_type_name(property)))));
        info.set(name, property.as_double());
    }
    return info;
}

// The name a method takes first, when it's given one.
static JS::ThrowCompletionOr<JsonValue> parse_alarm_name(JS::VM& vm, AlarmMethod const& method, JS::Value value)
{
    if (value.is_undefined())
        return JsonValue {};
    if (!value.is_string()) {
        TRY(throw_alarm_error(vm, method, MUST(String::formatted("Error at parameter 'name': Invalid type: expected string, found {}.", api_type_name(value)))));
        VERIFY_NOT_REACHED();
    }
    return JsonValue { value.as_string().utf16_string().to_utf8() };
}

static JS::Value call_alarm_method(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, AlarmMethod const& method, JsonArray const& arguments)
{
    return settle_api_call(context, callback, MessageRouter::the().call_api(context, MUST(String::formatted("alarms.{}", method.name)), arguments));
}

GC::Ref<JS::Object> create_alarms(JS::Realm& realm, MessagingContext& context)
{
    auto alarms = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    alarms->define_native_function(realm, "create"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        static constexpr AlarmMethod method { "create"sv, "optional string name, object alarmInfo, optional function callback"sv };
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto count = vm.argument_count();
        GC::Ptr<JS::FunctionObject> callback;
        if (count > 1 && vm.argument(count - 1).is_function())
            callback = vm.argument(--count).as_function();
        if (count > 2)
            TRY(throw_alarm_error(vm, method, "No matching signature."sv));
        // The name is optional, so a single argument is the alarm's info.
        JsonArray arguments;
        arguments.must_append(count == 2 ? TRY(parse_alarm_name(vm, method, vm.argument(0))) : JsonValue {});
        arguments.must_append(TRY(parse_alarm_info(vm, method, vm.argument(count == 2 ? 1 : 0))));
        return call_alarm_method(*context, callback, method, arguments); }, 1, attributes);

    for (auto name : { "get"sv, "clear"sv }) {
        alarms->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            AlarmMethod method { name, "optional string name, optional function callback"sv };
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count > 0 && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            if (count > 1)
                TRY(throw_alarm_error(vm, method, "No matching signature."sv));
            // With the callback alone, the alarm is the unnamed one.
            JsonArray arguments;
            arguments.must_append(count == 1 ? TRY(parse_alarm_name(vm, method, vm.argument(0))) : JsonValue {});
            return call_alarm_method(*context, callback, method, arguments); }, 0, attributes);
    }

    for (auto name : { "getAll"sv, "clearAll"sv }) {
        alarms->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            AlarmMethod method { name, "optional function callback"sv };
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument_count() > 0 && vm.argument(0).is_function())
                callback = vm.argument(0).as_function();
            else if (vm.argument_count() > 0)
                TRY(throw_alarm_error(vm, method, "No matching signature."sv));
            return call_alarm_method(*context, callback, method, JsonArray {}); }, 0, attributes);
    }

    alarms->define_direct_property("onAlarm"_utf16_fly_string, create_event(realm, context.event_listeners("alarms.onAlarm"_string)), attributes);
    return alarms;
}

}
