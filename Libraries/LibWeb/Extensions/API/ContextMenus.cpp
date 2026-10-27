/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/ContextMenus.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_menus_error(JS::VM& vm, StringView method, StringView signature, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of contextMenus.{}({}): {}", method, signature, detail));
}

// The properties as JSON, which leaves out the onclick handler manifest version 2 allows: Nothing calls it, since no
// menu is shown.
static JS::ThrowCompletionOr<JsonObject> properties_of(JS::VM& vm, StringView method, StringView signature, StringView parameter, JS::Value value)
{
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object()))
        TRY(throw_menus_error(vm, method, signature, MUST(String::formatted("Error at parameter '{}': Invalid type: expected object, found {}.", parameter, api_type_name(value)))));
    auto json = TRY(JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::js_undefined()));
    auto parsed = json.has_value() ? JsonValue::from_string(json->to_utf8()) : ErrorOr<JsonValue> { Error::from_string_literal("unserializable") };
    if (parsed.is_error() || !parsed.value().is_object())
        return JsonObject {};
    return parsed.value().as_object();
}

static JS::ThrowCompletionOr<JsonValue> id_of(JS::VM& vm, StringView method, StringView signature, StringView parameter, JS::Value value)
{
    if (value.is_string())
        return JsonValue { value.as_string().utf16_string().to_utf8() };
    if (is_api_integer(value))
        return JsonValue { static_cast<i64>(value.as_double()) };
    TRY(throw_menus_error(vm, method, signature, MUST(String::formatted("Error at parameter '{}': Value did not match any choice.", parameter))));
    VERIFY_NOT_REACHED();
}

// An item created without an id gets the next integer for its extension in this process, from 1.
static i64 next_generated_id(String const& extension_id)
{
    static auto& next_ids = *new HashMap<String, i64>;
    auto& next = next_ids.ensure(extension_id, [] { return 1; });
    return next++;
}

GC::Ref<JS::Object> create_context_menus(JS::Realm& realm, MessagingContext& context)
{
    auto menus = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    menus->define_direct_property("ACTION_MENU_TOP_LEVEL_LIMIT"_utf16_fly_string, JS::Value(6), 0);
    // No menu is shown yet, so nothing is ever clicked.
    menus->define_direct_property("onClicked"_utf16_fly_string, create_event(realm, context.event_listeners("contextMenus.onClicked"_string)), attributes);

    // create() returns the item's id at once, and reports a failure to its callback through runtime.lastError.
    menus->define_native_function(realm, "create"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        static constexpr auto signature = "object createProperties, optional function callback"sv;
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(1).is_function())
            callback = vm.argument(1).as_function();
        auto properties = TRY(properties_of(vm, "create"sv, signature, "createProperties"sv, vm.argument(0)));
        JS::Value id;
        if (auto given = TRY(vm.argument(0).as_object().get("id"_utf16_fly_string)); !given.is_undefined()) {
            if (!given.is_string())
                TRY(throw_menus_error(vm, "create"sv, signature, MUST(String::formatted("Error at parameter 'createProperties': Error at property 'id': Invalid type: expected string, found {}.", api_type_name(given)))));
            id = given;
        } else {
            auto generated = next_generated_id(context->descriptor().id);
            properties.set("id"sv, generated);
            id = JS::Value(static_cast<double>(generated));
        }
        JsonArray arguments;
        arguments.must_append(move(properties));
        (void)settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "contextMenus.create"sv, arguments));
        return id; }, 1, attributes);

    menus->define_native_function(realm, "update"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        static constexpr auto signature = "[integer|string] id, object updateProperties, optional function callback"sv;
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(2).is_function())
            callback = vm.argument(2).as_function();
        JsonArray arguments;
        arguments.must_append(TRY(id_of(vm, "update"sv, signature, "id"sv, vm.argument(0))));
        arguments.must_append(TRY(properties_of(vm, "update"sv, signature, "updateProperties"sv, vm.argument(1))));
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "contextMenus.update"sv, arguments)); }, 2, attributes);

    menus->define_native_function(realm, "remove"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        static constexpr auto signature = "[integer|string] menuItemId, optional function callback"sv;
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(1).is_function())
            callback = vm.argument(1).as_function();
        JsonArray arguments;
        arguments.must_append(TRY(id_of(vm, "remove"sv, signature, "menuItemId"sv, vm.argument(0))));
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "contextMenus.remove"sv, arguments)); }, 1, attributes);

    menus->define_native_function(realm, "removeAll"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(0).is_function())
            callback = vm.argument(0).as_function();
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "contextMenus.removeAll"sv, {})); }, 0, attributes);
    return menus;
}

}
