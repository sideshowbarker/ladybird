/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibGfx/Color.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Extensions/API/Action.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

namespace {

// One action method's name and the signature its errors quote.
struct Method {
    StringView api_namespace;
    StringView name;
    StringView signature;
};

// Extensions compare these messages, so their wording is fixed.
JS::ThrowCompletionOr<void> throw_invocation_error(JS::VM& vm, Method const& method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of {}.{}({}): {}", method.api_namespace, method.name, method.signature, detail));
}

JS::ThrowCompletionOr<void> throw_property_error(JS::VM& vm, Method const& method, StringView property, StringView expected, JS::Value value)
{
    return throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter 'details': Error at property '{}': Invalid type: expected {}, found {}.", property, expected, api_type_name(value))));
}

struct Details {
    GC::Ptr<JS::Object> object;
    JsonValue tab_id;
};

// Reads a details object and its optional tabId.
JS::ThrowCompletionOr<Details> parse_details(JS::VM& vm, Method const& method, JS::Value value)
{
    if (!value.is_object() || is<JS::Array>(value.as_object()) || value.is_function())
        TRY(throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter 'details': Invalid type: expected object, found {}.", api_type_name(value)))));
    Details details { value.as_object(), JsonValue {} };
    auto tab_id = TRY(details.object->get("tabId"_utf16_fly_string));
    if (!tab_id.is_undefined() && !tab_id.is_null()) {
        if (!is_api_integer(tab_id))
            TRY(throw_property_error(vm, method, "tabId"sv, "integer"sv, tab_id));
        details.tab_id = static_cast<i64>(tab_id.as_double());
    }
    return details;
}

JS::ThrowCompletionOr<JS::Value> get_property(JS::VM&, Details const& details, StringView name)
{
    return details.object->get(Utf16FlyString::from_utf8(name));
}

// A color is a CSS color string, or an [r, g, b, a] array of integers from 0 to 255; it travels as the array.
JS::ThrowCompletionOr<Optional<JsonArray>> color_from(JS::VM& vm, JS::Value value)
{
    Optional<Gfx::Color> color;
    if (value.is_string()) {
        color = Gfx::Color::from_string(value.as_string().utf16_string().to_utf8());
    } else if (value.is_object() && is<JS::Array>(value.as_object())) {
        auto& array = static_cast<JS::Array&>(value.as_object());
        // The array's elements can be getters, which can throw.
        auto length = TRY(JS::length_of_array_like(vm, array));
        if (length != 4)
            return OptionalNone {};
        u8 components[4];
        for (size_t i = 0; i < 4; ++i) {
            auto component = TRY(array.get(JS::PropertyKey { i }));
            if (!component.is_integral_number() || component.as_double() < 0 || component.as_double() > 255)
                return OptionalNone {};
            components[i] = static_cast<u8>(component.as_double());
        }
        color = Gfx::Color { components[0], components[1], components[2], components[3] };
    }
    if (!color.has_value())
        return OptionalNone {};
    JsonArray array;
    for (auto component : { color->red(), color->green(), color->blue(), color->alpha() })
        array.must_append(component);
    return array;
}

JS::Value reject(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, StringView message)
{
    auto& realm = context.realm();
    return settle_api_call(context, callback, WebIDL::create_rejected_promise(realm, JS::Error::create(realm, Utf16String::from_utf8(message))));
}

JS::Value call(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JsonValue tab_id, Optional<JsonValue> value = {})
{
    JsonArray arguments;
    arguments.must_append(move(tab_id));
    if (value.has_value())
        arguments.must_append(value.release_value());
    auto qualified = MUST(String::formatted("{}.{}", method.api_namespace, method.name));
    return settle_api_call(context, callback, MessageRouter::the().call_api(context, qualified, arguments));
}

// Splits the arguments into the given ones and a trailing callback.
GC::Ptr<JS::FunctionObject> trailing_callback(JS::VM& vm, size_t& count)
{
    count = vm.argument_count();
    if (count > 0 && vm.argument(count - 1).is_function())
        return vm.argument(--count).as_function();
    return nullptr;
}

}

GC::Ref<JS::Object> create_action(JS::Realm& realm, MessagingContext& context)
{
    auto api_namespace = context.descriptor().manifest_version == 2 ? "browserAction"sv : "action"sv;
    auto action = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    static constexpr auto details_signature = "object details, optional function callback"sv;

    auto define = [&](StringView name, auto handler) {
        Method method { api_namespace, name, details_signature };
        action->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, method, handler](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            return handler(vm, *context, callback, method, count > 0 ? vm.argument(0) : JS::js_undefined()); }, 1, attributes);
    };

    // A setter whose value is a string, required unless the property may be cleared by leaving it out.
    auto define_string_setter = [&](StringView name, StringView property, bool required) {
        define(name, [property, required](JS::VM& vm, MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JS::Value argument) -> JS::ThrowCompletionOr<JS::Value> {
            auto details = TRY(parse_details(vm, method, argument));
            auto value = TRY(get_property(vm, details, property));
            if (value.is_undefined() || value.is_null()) {
                if (required)
                    TRY(throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter 'details': Missing required property '{}'.", property))));
                return call(context, callback, method, move(details.tab_id), JsonValue {});
            }
            if (!value.is_string())
                TRY(throw_property_error(vm, method, property, "string"sv, value));
            return call(context, callback, method, move(details.tab_id), JsonValue { value.as_string().utf16_string().to_utf8() });
        });
    };

    auto define_getter = [&](StringView name) {
        define(name, [](JS::VM& vm, MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JS::Value argument) -> JS::ThrowCompletionOr<JS::Value> {
            auto details = TRY(parse_details(vm, method, argument));
            return call(context, callback, method, move(details.tab_id));
        });
    };

    auto define_color_setter = [&](StringView name) {
        define(name, [](JS::VM& vm, MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JS::Value argument) -> JS::ThrowCompletionOr<JS::Value> {
            auto details = TRY(parse_details(vm, method, argument));
            auto value = TRY(get_property(vm, details, "color"sv));
            if (value.is_undefined())
                TRY(throw_invocation_error(vm, method, "Error at parameter 'details': Missing required property 'color'."sv));
            auto color = TRY(color_from(vm, value));
            if (!color.has_value())
                return reject(context, callback, "Invalid color"sv);
            return call(context, callback, method, move(details.tab_id), JsonValue { color.release_value() });
        });
    };

    define_string_setter("setTitle"sv, "title"sv, true);
    define_getter("getTitle"sv);
    define_string_setter("setPopup"sv, "popup"sv, true);
    define_getter("getPopup"sv);
    define_string_setter("setBadgeText"sv, "text"sv, false);
    define_getter("getBadgeText"sv);
    define_color_setter("setBadgeBackgroundColor"sv);
    define_getter("getBadgeBackgroundColor"sv);
    // Only manifest version 3's action has a badge text color.
    if (api_namespace == "action"sv) {
        define_color_setter("setBadgeTextColor"sv);
        define_getter("getBadgeTextColor"sv);
    }

    define("setIcon"sv, [](JS::VM& vm, MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JS::Value argument) -> JS::ThrowCompletionOr<JS::Value> {
        auto details = TRY(parse_details(vm, method, argument));
        if (!TRY(get_property(vm, details, "imageData"sv)).is_undefined())
            return reject(context, callback, "imageData isn't supported yet"sv);
        auto path = TRY(get_property(vm, details, "path"sv));
        if (path.is_string())
            return call(context, callback, method, move(details.tab_id), JsonValue { path.as_string().utf16_string().to_utf8() });
        if (path.is_object() && !is<JS::Array>(path.as_object()) && !path.is_function()) {
            JsonObject sizes;
            for (auto const& property_key : TRY(path.as_object().internal_own_property_keys())) {
                auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
                if (key.is_symbol())
                    continue;
                auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
                auto size_path = TRY(path.as_object().get(key));
                if (!size_path.is_string())
                    TRY(throw_property_error(vm, method, "path"sv, "string"sv, size_path));
                sizes.set(name, size_path.as_string().utf16_string().to_utf8());
            }
            return call(context, callback, method, move(details.tab_id), JsonValue { move(sizes) });
        }
        if (!path.is_undefined())
            TRY(throw_property_error(vm, method, "path"sv, "string or object"sv, path));
        return reject(context, callback, "Either path or imageData must be specified"sv);
    });

    // enable(), disable(), and isEnabled() take a tab id rather than a details object.
    for (auto name : { "enable"sv, "disable"sv, "isEnabled"sv }) {
        Method method { api_namespace, name, "optional integer tabId, optional function callback"sv };
        action->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, method](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            JsonValue tab_id;
            if (count > 0 && !vm.argument(0).is_undefined() && !vm.argument(0).is_null()) {
                if (!is_api_integer(vm.argument(0)))
                    TRY(throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter 'tabId': Invalid type: expected integer, found {}.", api_type_name(vm.argument(0))))));
                tab_id = static_cast<i64>(vm.argument(0).as_double());
            }
            return call(*context, callback, method, move(tab_id)); }, 0, attributes);
    }

    auto on_clicked = MUST(String::formatted("{}.onClicked", api_namespace));
    action->define_direct_property("onClicked"_utf16_fly_string, create_event(realm, context.event_listeners(on_clicked)), attributes);
    return action;
}

}
