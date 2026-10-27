/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Arguments.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Scripting.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_execute_script_error(JS::VM& vm, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of scripting.executeScript(scripting.ScriptInjection injection, optional function callback): {}", detail));
}

static bool is_plain_object(JS::Value value)
{
    return value.is_object() && !value.is_function() && !is<JS::Array>(value.as_object());
}

// The injection as the UI process reads it: its target, its files or its function's source and arguments as JSON, and
// its world. Whether it names exactly one of the two is the UI process's check.
static JS::ThrowCompletionOr<JsonObject> parse_injection(JS::VM& vm, JS::Value value)
{
    if (!is_plain_object(value)) {
        TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Invalid type: expected scripting.ScriptInjection, found {}.", api_type_name(value)))));
        VERIFY_NOT_REACHED();
    }
    auto& injection = value.as_object();
    JsonObject details;

    auto target_value = TRY(injection.get("target"_utf16_fly_string));
    if (!is_plain_object(target_value))
        TRY(throw_execute_script_error(vm, "Error at parameter 'injection': Missing required property 'target'."sv));
    auto& target = target_value.as_object();
    JsonObject target_details;
    auto tab_id = TRY(target.get("tabId"_utf16_fly_string));
    if (!is_api_integer(tab_id))
        TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Error at property 'target': Error at property 'tabId': Invalid type: expected integer, found {}.", api_type_name(tab_id)))));
    target_details.set("tabId"sv, static_cast<i64>(tab_id.as_double()));
    if (auto all_frames = TRY(target.get("allFrames"_utf16_fly_string)); all_frames.is_boolean())
        target_details.set("allFrames"sv, all_frames.as_bool());
    auto frame_ids = TRY(target.get("frameIds"_utf16_fly_string));
    if (!frame_ids.is_undefined() && (!frame_ids.is_object() || !is<JS::Array>(frame_ids.as_object())))
        TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Error at property 'target': Error at property 'frameIds': Invalid type: expected array, found {}.", api_type_name(frame_ids)))));
    if (!frame_ids.is_undefined()) {
        JsonArray ids;
        auto length = TRY(JS::length_of_array_like(vm, frame_ids.as_object()));
        for (size_t i = 0; i < length; ++i) {
            auto id = TRY(frame_ids.as_object().get(JS::PropertyKey { i }));
            if (!is_api_integer(id))
                TRY(throw_execute_script_error(vm, "Error at parameter 'injection': Error at property 'target': Error at property 'frameIds': Invalid type: expected integer."sv));
            ids.must_append(static_cast<i64>(id.as_double()));
        }
        target_details.set("frameIds"sv, move(ids));
    }
    if (!TRY(target.get("documentIds"_utf16_fly_string)).is_undefined())
        target_details.set("documentIds"sv, JsonArray {});
    details.set("target"sv, move(target_details));

    // A function travels as its source, and so it can't see anything it closes over.
    if (auto func = TRY(injection.get("func"_utf16_fly_string)); !func.is_undefined()) {
        if (!func.is_function())
            TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Error at property 'func': Invalid type: expected function, found {}.", api_type_name(func)))));
        details.set("func"sv, TRY(func.to_utf16_string(vm)).to_utf8_but_should_be_ported_to_utf16());
        auto args = TRY(injection.get("args"_utf16_fly_string));
        if (!args.is_undefined()) {
            auto json = TRY(JS::JSONObject::stringify_impl(vm, args, JS::js_undefined(), JS::js_undefined()));
            if (!args.is_object() || !is<JS::Array>(args.as_object()) || !json.has_value())
                TRY(throw_execute_script_error(vm, "Error at parameter 'injection': Error at property 'args': Invalid type: expected array."sv));
            details.set("args"sv, json->to_utf8());
        }
    }
    if (auto files = TRY(injection.get("files"_utf16_fly_string)); !files.is_undefined()) {
        if (!files.is_object() || !is<JS::Array>(files.as_object()))
            TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Error at property 'files': Invalid type: expected array, found {}.", api_type_name(files)))));
        JsonArray paths;
        auto length = TRY(JS::length_of_array_like(vm, files.as_object()));
        for (size_t i = 0; i < length; ++i) {
            auto path = TRY(files.as_object().get(JS::PropertyKey { i }));
            if (!path.is_string())
                TRY(throw_execute_script_error(vm, "Error at parameter 'injection': Error at property 'files': Invalid type: expected string."sv));
            paths.must_append(path.as_string().utf16_string().to_utf8());
        }
        details.set("files"sv, move(paths));
    }
    if (auto immediately = TRY(injection.get("injectImmediately"_utf16_fly_string)); !immediately.is_undefined()) {
        if (!immediately.is_boolean())
            TRY(throw_execute_script_error(vm, MUST(String::formatted("Error at parameter 'injection': Error at property 'injectImmediately': Invalid type: expected boolean, found {}.", api_type_name(immediately)))));
        details.set("injectImmediately"sv, immediately.as_bool());
    }
    if (auto world = TRY(injection.get("world"_utf16_fly_string)); world.is_string()) {
        auto name = world.as_string().utf16_string().to_utf8();
        if (name != "ISOLATED"sv && name != "MAIN"sv)
            TRY(throw_execute_script_error(vm, "Error at parameter 'injection': Error at property 'world': Value must be one of ISOLATED, MAIN."sv));
        details.set("world"sv, name);
    }
    return details;
}

GC::Ref<JS::Object> create_scripting(JS::Realm& realm, MessagingContext& context)
{
    auto scripting = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    scripting->define_native_function(realm, "executeScript"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(1).is_function())
            callback = vm.argument(1).as_function();
        JsonArray arguments;
        arguments.must_append(TRY(parse_injection(vm, vm.argument(0))));
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "scripting.executeScript"sv, arguments)); }, 1, attributes);
    return scripting;
}

}
