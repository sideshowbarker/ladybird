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
#include <LibWeb/Extensions/API/Windows.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_windows_error(JS::VM& vm, StringView method, StringView signature, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of windows.{}({}): {}", method, signature, detail));
}

// The query options every windows method takes: populate, and the window types that count.
static JS::ThrowCompletionOr<JsonObject> parse_query_options(JS::VM& vm, StringView method, StringView signature, JS::Value value)
{
    JsonObject options;
    if (value.is_nullish())
        return options;
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object()))
        TRY(throw_windows_error(vm, method, signature, MUST(String::formatted("Error at parameter 'queryOptions': Invalid type: expected object, found {}.", api_type_name(value)))));
    if (auto populate = TRY(value.as_object().get("populate"_utf16_fly_string)); populate.is_boolean())
        options.set("populate"sv, populate.as_bool());
    if (auto types = TRY(value.as_object().get("windowTypes"_utf16_fly_string)); types.is_object() && is<JS::Array>(types.as_object())) {
        JsonArray names;
        auto length = TRY(JS::length_of_array_like(vm, types.as_object()));
        for (size_t i = 0; i < length; ++i) {
            if (auto type = TRY(types.as_object().get(JS::PropertyKey { i })); type.is_string())
                names.must_append(type.as_string().utf16_string().to_utf8());
        }
        options.set("windowTypes"sv, move(names));
    }
    return options;
}

GC::Ref<JS::Object> create_windows(JS::Realm& realm, MessagingContext& context)
{
    auto windows = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    windows->define_direct_property("WINDOW_ID_NONE"_utf16_fly_string, JS::Value(-1), 0);
    windows->define_direct_property("WINDOW_ID_CURRENT"_utf16_fly_string, JS::Value(-2), 0);
    // Nothing reports a window opening, closing, or gaining focus yet, so these never fire.
    for (auto name : { "onCreated"sv, "onRemoved"sv, "onFocusChanged"sv })
        windows->define_direct_property(Utf16FlyString::from_utf8(name), create_event(realm, context.event_listeners(MUST(String::formatted("windows.{}", name)))), attributes);

    windows->define_native_function(realm, "get"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        static constexpr auto signature = "integer windowId, optional object queryOptions, optional function callback"sv;
        auto count = vm.argument_count();
        GC::Ptr<JS::FunctionObject> callback;
        if (count > 1 && vm.argument(count - 1).is_function())
            callback = vm.argument(--count).as_function();
        if (!is_api_integer(vm.argument(0)))
            TRY(throw_windows_error(vm, "get"sv, signature, MUST(String::formatted("Error at parameter 'windowId': Invalid type: expected integer, found {}.", api_type_name(vm.argument(0))))));
        JsonArray arguments;
        arguments.must_append(static_cast<i64>(vm.argument(0).as_double()));
        arguments.must_append(TRY(parse_query_options(vm, "get"sv, signature, count > 1 ? vm.argument(1) : JS::js_undefined())));
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "windows.get"sv, arguments)); }, 1, attributes);

    for (auto method : { "getAll"sv, "getCurrent"sv, "getLastFocused"sv }) {
        windows->define_native_function(realm, Utf16FlyString::from_utf8(method), [context = GC::Ref { context }, method](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            static constexpr auto signature = "optional object queryOptions, optional function callback"sv;
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count > 0 && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            JsonArray arguments;
            arguments.must_append(TRY(parse_query_options(vm, method, signature, count > 0 ? vm.argument(0) : JS::js_undefined())));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("windows.{}", method)), arguments)); }, 0, attributes);
    }
    return windows;
}

}
