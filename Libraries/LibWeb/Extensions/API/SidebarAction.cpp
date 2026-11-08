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
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/SidePanel.h>
#include <LibWeb/Extensions/API/SidebarAction.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

namespace {

// Extensions compare these messages, so their wording is fixed.
JS::ThrowCompletionOr<void> throw_details_error(JS::VM& vm, StringView method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of sidebarAction.{}(object details, optional function callback): {}", method, detail));
}

// Reads a details object: its tabId and windowId, and the one property the method is about, if any.
JS::ThrowCompletionOr<JsonObject> parse_details(JS::VM& vm, StringView method, JS::Value value, bool required, Optional<StringView> property, StringView property_type)
{
    JsonObject details;
    if (value.is_undefined() || value.is_null()) {
        if (required)
            TRY(throw_details_error(vm, method, "No matching signature."sv));
        return details;
    }
    if (!value.is_object() || is<JS::Array>(value.as_object()) || value.is_function())
        TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Invalid type: expected object, found {}.", api_type_name(value)))));
    auto& object = value.as_object();
    for (auto id : { "tabId"sv, "windowId"sv }) {
        auto id_value = TRY(object.get(Utf16FlyString::from_utf8(id)));
        if (id_value.is_undefined() || id_value.is_null())
            continue;
        if (!is_api_integer(id_value))
            TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Error at property '{}': Invalid type: expected integer, found {}.", id, api_type_name(id_value)))));
        details.set(id, static_cast<i64>(id_value.as_double()));
    }
    if (!property.has_value())
        return details;
    auto property_value = TRY(object.get(Utf16FlyString::from_utf8(*property)));
    // A null value restores the manifest's; the property has to be there, though.
    if (property_value.is_undefined())
        TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Missing required property '{}'.", *property))));
    if (property_value.is_null()) {
        details.set(*property, JsonValue {});
    } else if (property_type == "string"sv) {
        if (!property_value.is_string())
            TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Error at property '{}': Invalid type: expected string, found {}.", *property, api_type_name(property_value)))));
        details.set(*property, property_value.as_string().utf16_string().to_utf8());
    } else if (property_type == "path"sv) {
        // An icon is a path, or a map from sizes to paths.
        if (property_value.is_string()) {
            details.set(*property, property_value.as_string().utf16_string().to_utf8());
        } else if (property_value.is_object() && !is<JS::Array>(property_value.as_object()) && !property_value.is_function()) {
            JsonObject sizes;
            for (auto const& property_key : TRY(property_value.as_object().internal_own_property_keys())) {
                auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
                if (key.is_symbol())
                    continue;
                auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
                auto size_path = TRY(property_value.as_object().get(key));
                if (!size_path.is_string())
                    TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Error at property '{}': Invalid type: expected string, found {}.", *property, api_type_name(size_path)))));
                sizes.set(name, size_path.as_string().utf16_string().to_utf8());
            }
            details.set(*property, move(sizes));
        } else {
            TRY(throw_details_error(vm, method, MUST(String::formatted("Error at parameter 'details': Error at property '{}': Invalid type: expected string or object, found {}.", *property, api_type_name(property_value)))));
        }
    }
    return details;
}

JS::Value reject(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, StringView message)
{
    auto& realm = context.realm();
    return settle_api_call(context, callback, WebIDL::create_rejected_promise(realm, JS::Error::create(realm, Utf16String::from_utf8(message))));
}

JS::Value call(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, StringView method, JsonArray arguments)
{
    return settle_api_call(context, callback, MessageRouter::the().call_api(context, MUST(String::formatted("sidebarAction.{}", method)), arguments));
}

GC::Ptr<JS::FunctionObject> trailing_callback(JS::VM& vm, size_t& count)
{
    count = vm.argument_count();
    if (count > 0 && vm.argument(count - 1).is_function())
        return vm.argument(--count).as_function();
    return nullptr;
}

}

GC::Ref<JS::Object> create_sidebar_action(JS::Realm& realm, MessagingContext& context)
{
    auto sidebar_action = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    // A method that takes a details object naming a tab or a window, and for a setter the property it sets.
    auto define_with_details = [&](StringView name, bool required, Optional<StringView> property, StringView property_type) {
        sidebar_action->define_native_function(realm, Utf16FlyString::from_utf8(name), GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, MUST(String::from_utf8(name)), required, property.map([](StringView view) { return MUST(String::from_utf8(view)); }), MUST(String::from_utf8(property_type))), [](auto& context, auto& name_string, auto& required, auto& property_string, auto& property_type_string, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto name = name_string.bytes_as_string_view();
            auto property_type = property_type_string.bytes_as_string_view();
            auto property = property_string.map([](String const& string) { return string.bytes_as_string_view(); });
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            auto argument = count > 0 ? vm.argument(0) : JS::js_undefined();
            if (name == "setIcon"sv && argument.is_object() && !TRY(argument.as_object().get("imageData"_utf16_fly_string)).is_undefined())
                return reject(*context, callback, "imageData isn't supported yet"sv);
            auto details = TRY(parse_details(vm, name, argument, required, property, property_type));
            if (details.has("tabId"sv) && details.has("windowId"sv))
                return reject(*context, callback, "Only one of tabId and windowId can be specified."sv);
            JsonArray arguments;
            arguments.must_append(move(details));
            return call(*context, callback, name, move(arguments)); }), required ? 1 : 0, attributes);
    };
    // open(), close(), and toggle() take no arguments, act on the current window, and need the user's input.
    auto define_user_input = [&](StringView name) {
        sidebar_action->define_native_function(realm, Utf16FlyString::from_utf8(name), GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, MUST(String::from_utf8(name))), [](auto& context, auto& name_string, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto name = name_string.bytes_as_string_view();
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            size_t count = 0;
            auto callback = trailing_callback(vm, count);
            if (!has_user_activation_for_side_panel(*context))
                return reject(*context, callback, MUST(String::formatted("sidebarAction.{} may only be called from a user input handler", name)));
            return call(*context, callback, name, JsonArray {}); }), 0, attributes);
    };

    define_with_details("setPanel"sv, true, "panel"sv, "string"sv);
    define_with_details("getPanel"sv, true, {}, {});
    define_with_details("setTitle"sv, true, "title"sv, "string"sv);
    define_with_details("getTitle"sv, true, {}, {});
    define_with_details("setIcon"sv, true, "path"sv, "path"sv);
    define_with_details("isOpen"sv, false, {}, {});
    define_user_input("open"sv);
    define_user_input("close"sv);
    define_user_input("toggle"sv);
    return sidebar_action;
}

}
