/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/SidePanel.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

bool has_user_activation_for_side_panel(MessagingContext& context)
{
    auto window = context.document().window();
    return window && window->has_transient_activation();
}

namespace {

struct Method {
    GC_HOLDS_NO_CELL_EDGES;
    StringView name;
    String signature;
};

// Extensions compare these messages, so their wording is fixed.
JS::ThrowCompletionOr<void> throw_invocation_error(JS::VM& vm, Method const& method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of sidePanel.{}({}): {}", method.name, method.signature, detail));
}

enum class PropertyType : u8 {
    Integer,
    String,
    Boolean,
};

struct Property {
    GC_HOLDS_NO_CELL_EDGES;
    StringView name;
    PropertyType type;
};

// Reads the properties of an options object that the API knows into JSON; a wrong type is an error naming it.
JS::ThrowCompletionOr<JsonObject> parse_options(JS::VM& vm, Method const& method, StringView parameter, JS::Value value, bool required, ReadonlySpan<Property> properties)
{
    JsonObject options;
    if (value.is_undefined() || value.is_null()) {
        if (required)
            TRY(throw_invocation_error(vm, method, "No matching signature."sv));
        return options;
    }
    if (!value.is_object() || is<JS::Array>(value.as_object()) || value.is_function())
        TRY(throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter '{}': Invalid type: expected object, found {}.", parameter, api_type_name(value)))));
    for (auto const& property : properties) {
        auto property_value = TRY(value.as_object().get(Utf16FlyString::from_utf8(property.name)));
        if (property_value.is_undefined() || property_value.is_null())
            continue;
        auto wrong_type = [&](StringView expected) {
            return throw_invocation_error(vm, method, MUST(String::formatted("Error at parameter '{}': Error at property '{}': Invalid type: expected {}, found {}.", parameter, property.name, expected, api_type_name(property_value))));
        };
        switch (property.type) {
        case PropertyType::Integer:
            if (!is_api_integer(property_value))
                TRY(wrong_type("integer"sv));
            options.set(property.name, static_cast<i64>(property_value.as_double()));
            break;
        case PropertyType::String:
            if (!property_value.is_string())
                TRY(wrong_type("string"sv));
            options.set(property.name, property_value.as_string().utf16_string().to_utf8());
            break;
        case PropertyType::Boolean:
            if (!property_value.is_boolean())
                TRY(wrong_type("boolean"sv));
            options.set(property.name, property_value.as_bool());
            break;
        }
    }
    return options;
}

JS::Value reject(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, StringView message)
{
    auto& realm = context.realm();
    return settle_api_call(context, callback, WebIDL::create_rejected_promise(realm, JS::Error::create(realm, Utf16String::from_utf8(message))));
}

JS::Value call(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, Method const& method, JsonArray arguments)
{
    return settle_api_call(context, callback, MessageRouter::the().call_api(context, MUST(String::formatted("sidePanel.{}", method.name)), arguments));
}

// Splits the arguments into the given ones and a trailing callback.
GC::Ptr<JS::FunctionObject> trailing_callback(JS::VM& vm, size_t& count)
{
    count = vm.argument_count();
    if (count > 0 && vm.argument(count - 1).is_function())
        return vm.argument(--count).as_function();
    return nullptr;
}

constexpr Array panel_options_properties { Property { "tabId"sv, PropertyType::Integer }, Property { "path"sv, PropertyType::String }, Property { "enabled"sv, PropertyType::Boolean } };
constexpr Array get_options_properties { Property { "tabId"sv, PropertyType::Integer } };
constexpr Array behavior_properties { Property { "openPanelOnActionClick"sv, PropertyType::Boolean } };
constexpr Array open_options_properties { Property { "windowId"sv, PropertyType::Integer }, Property { "tabId"sv, PropertyType::Integer } };

}

GC::Ref<JS::Object> create_side_panel(JS::Realm& realm, MessagingContext& context)
{
    auto side_panel = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    for (auto name : { "onOpened"sv, "onClosed"sv })
        side_panel->define_direct_property(Utf16FlyString::from_utf8(name), create_event(realm, context.event_listeners(MUST(String::formatted("sidePanel.{}", name)))), attributes);

    // A method whose one argument is an options object, required or not, with the properties the API knows.
    auto define_with_options = [&](StringView name, StringView parameter, bool required, ReadonlySpan<Property> properties, bool needs_user_activation = false) {
        Method method { name, required ? MUST(String::formatted("object {}, optional function callback", parameter)) : MUST(String::formatted("optional object {}, optional function callback", parameter)) };
        side_panel->define_native_function(realm, Utf16FlyString::from_utf8(name), GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, method, MUST(String::from_utf8(parameter)), required, Vector<Property> { properties }, needs_user_activation), [](auto& context, auto& method, auto& parameter_string, auto& required, auto& properties_vector, auto& needs_user_activation, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto parameter = parameter_string.bytes_as_string_view();
            ReadonlySpan<Property> properties = properties_vector;
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            auto options = TRY(parse_options(vm, method, parameter, count > 0 ? vm.argument(0) : JS::js_undefined(), required, properties));
            if (needs_user_activation && !has_user_activation_for_side_panel(*context))
                return reject(*context, callback, "`sidePanel.open()` may only be called in response to a user gesture."sv);
            JsonArray arguments;
            arguments.must_append(move(options));
            return call(*context, callback, method, move(arguments)); }), required ? 1 : 0, attributes);
    };
    // A method without arguments.
    auto define_plain = [&](StringView name) {
        Method method { name, "optional function callback"_string };
        side_panel->define_native_function(realm, Utf16FlyString::from_utf8(name), GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, method), [](auto& context, auto& method, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            return call(*context, callback, method, JsonArray {}); }), 0, attributes);
    };

    define_with_options("setOptions"sv, "options"sv, true, panel_options_properties);
    define_with_options("getOptions"sv, "options"sv, false, get_options_properties);
    define_with_options("setPanelBehavior"sv, "behavior"sv, true, behavior_properties);
    define_plain("getPanelBehavior"sv);
    define_with_options("open"sv, "options"sv, true, open_options_properties, true);
    define_with_options("close"sv, "options"sv, true, open_options_properties);
    define_plain("getLayout"sv);
    return side_panel;
}

}
