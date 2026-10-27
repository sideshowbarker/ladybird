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
static constexpr auto execute_script_signature = "executeScript(scripting.ScriptInjection injection, optional function callback)"sv;
static constexpr auto insert_css_signature = "insertCSS(scripting.CSSInjection injection, optional function callback)"sv;
static constexpr auto remove_css_signature = "removeCSS(scripting.CSSInjection injection, optional function callback)"sv;

static JS::ThrowCompletionOr<void> throw_invocation_error(JS::VM& vm, StringView signature, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of scripting.{}: {}", signature, detail));
}

static JS::ThrowCompletionOr<void> throw_execute_script_error(JS::VM& vm, StringView detail)
{
    return throw_invocation_error(vm, execute_script_signature, detail);
}

static bool is_plain_object(JS::Value value)
{
    return value.is_object() && !value.is_function() && !is<JS::Array>(value.as_object());
}

// The injection's target, which every scripting method reads the same way.
static JS::ThrowCompletionOr<JsonObject> parse_target(JS::VM& vm, StringView signature, JS::Object& injection)
{
    auto target_value = TRY(injection.get("target"_utf16_fly_string));
    if (!is_plain_object(target_value))
        TRY(throw_invocation_error(vm, signature, "Error at parameter 'injection': Missing required property 'target'."sv));
    auto& target = target_value.as_object();
    JsonObject target_details;
    auto tab_id = TRY(target.get("tabId"_utf16_fly_string));
    if (!is_api_integer(tab_id))
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'injection': Error at property 'target': Error at property 'tabId': Invalid type: expected integer, found {}.", api_type_name(tab_id)))));
    target_details.set("tabId"sv, static_cast<i64>(tab_id.as_double()));
    if (auto all_frames = TRY(target.get("allFrames"_utf16_fly_string)); all_frames.is_boolean())
        target_details.set("allFrames"sv, all_frames.as_bool());
    auto frame_ids = TRY(target.get("frameIds"_utf16_fly_string));
    if (!frame_ids.is_undefined() && (!frame_ids.is_object() || !is<JS::Array>(frame_ids.as_object())))
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'injection': Error at property 'target': Error at property 'frameIds': Invalid type: expected array, found {}.", api_type_name(frame_ids)))));
    if (!frame_ids.is_undefined()) {
        JsonArray ids;
        auto length = TRY(JS::length_of_array_like(vm, frame_ids.as_object()));
        for (size_t i = 0; i < length; ++i) {
            auto id = TRY(frame_ids.as_object().get(JS::PropertyKey { i }));
            if (!is_api_integer(id))
                TRY(throw_invocation_error(vm, signature, "Error at parameter 'injection': Error at property 'target': Error at property 'frameIds': Invalid type: expected integer."sv));
            ids.must_append(static_cast<i64>(id.as_double()));
        }
        target_details.set("frameIds"sv, move(ids));
    }
    if (!TRY(target.get("documentIds"_utf16_fly_string)).is_undefined())
        target_details.set("documentIds"sv, JsonArray {});
    return target_details;
}

static JS::ThrowCompletionOr<void> parse_files(JS::VM& vm, StringView signature, JS::Object& injection, JsonObject& details)
{
    if (auto files = TRY(injection.get("files"_utf16_fly_string)); !files.is_undefined()) {
        if (!files.is_object() || !is<JS::Array>(files.as_object()))
            TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'injection': Error at property 'files': Invalid type: expected array, found {}.", api_type_name(files)))));
        JsonArray paths;
        auto length = TRY(JS::length_of_array_like(vm, files.as_object()));
        for (size_t i = 0; i < length; ++i) {
            auto path = TRY(files.as_object().get(JS::PropertyKey { i }));
            if (!path.is_string())
                TRY(throw_invocation_error(vm, signature, "Error at parameter 'injection': Error at property 'files': Invalid type: expected string."sv));
            paths.must_append(path.as_string().utf16_string().to_utf8());
        }
        details.set("files"sv, move(paths));
    }
    return {};
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

    details.set("target"sv, TRY(parse_target(vm, execute_script_signature, injection)));

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
    TRY(parse_files(vm, execute_script_signature, injection, details));
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

// A CSS injection: its target, its CSS or files, and its origin.
static JS::ThrowCompletionOr<JsonObject> parse_css_injection(JS::VM& vm, StringView signature, JS::Value value)
{
    if (!is_plain_object(value)) {
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'injection': Invalid type: expected scripting.CSSInjection, found {}.", api_type_name(value)))));
        VERIFY_NOT_REACHED();
    }
    auto& injection = value.as_object();
    JsonObject details;
    details.set("target"sv, TRY(parse_target(vm, signature, injection)));
    if (auto css = TRY(injection.get("css"_utf16_fly_string)); !css.is_undefined()) {
        if (!css.is_string())
            TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'injection': Error at property 'css': Invalid type: expected string, found {}.", api_type_name(css)))));
        details.set("css"sv, css.as_string().utf16_string().to_utf8());
    }
    TRY(parse_files(vm, signature, injection, details));
    if (auto origin = TRY(injection.get("origin"_utf16_fly_string)); !origin.is_undefined()) {
        auto name = origin.is_string() ? origin.as_string().utf16_string().to_utf8() : String {};
        if (name != "AUTHOR"sv && name != "USER"sv)
            TRY(throw_invocation_error(vm, signature, "Error at parameter 'injection': Error at property 'origin': Value must be one of AUTHOR, USER."sv));
        details.set("origin"sv, name);
    }
    return details;
}

static constexpr auto register_signature = "registerContentScripts(array scripts, optional function callback)"sv;
static constexpr auto update_signature = "updateContentScripts(array scripts, optional function callback)"sv;
static constexpr auto get_signature = "getRegisteredContentScripts(optional scripting.ContentScriptFilter filter, optional function callback)"sv;
static constexpr auto unregister_signature = "unregisterContentScripts(optional scripting.ContentScriptFilter filter, optional function callback)"sv;

static JS::ThrowCompletionOr<JsonArray> parse_strings(JS::VM& vm, StringView signature, StringView where, JS::Value value)
{
    if (!value.is_object() || !is<JS::Array>(value.as_object()))
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Invalid type: expected array, found {}.", where, api_type_name(value)))));
    JsonArray strings;
    auto length = TRY(JS::length_of_array_like(vm, value.as_object()));
    for (size_t i = 0; i < length; ++i) {
        auto item = TRY(value.as_object().get(JS::PropertyKey { i }));
        if (!item.is_string())
            TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Error at index {}: Invalid type: expected string, found {}.", where, i, api_type_name(item)))));
        strings.must_append(item.as_string().utf16_string().to_utf8());
    }
    return strings;
}

// The scripts of a register or update call, each with the fields the UI process reads: the update's id alone is
// required there, and the UI process checks what a registered script needs.
static JS::ThrowCompletionOr<JsonArray> parse_registered_scripts(JS::VM& vm, StringView signature, JS::Value value)
{
    if (!value.is_object() || !is<JS::Array>(value.as_object()))
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'scripts': Invalid type: expected array, found {}.", api_type_name(value)))));
    JsonArray scripts;
    auto length = TRY(JS::length_of_array_like(vm, value.as_object()));
    for (size_t i = 0; i < length; ++i) {
        auto where = MUST(String::formatted("Error at parameter 'scripts': Error at index {}", i));
        auto item = TRY(value.as_object().get(JS::PropertyKey { i }));
        if (!is_plain_object(item))
            TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Invalid type: expected scripting.RegisteredContentScript, found {}.", where, api_type_name(item)))));
        auto& object = item.as_object();
        JsonObject script;
        auto id = TRY(object.get("id"_utf16_fly_string));
        if (!id.is_string())
            TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Missing required property 'id'.", where))));
        script.set("id"sv, id.as_string().utf16_string().to_utf8());
        for (auto key : { "matches"sv, "excludeMatches"sv, "js"sv, "css"sv }) {
            auto property = TRY(object.get(Utf16FlyString::from_utf8(key)));
            if (!property.is_undefined())
                script.set(key, TRY(parse_strings(vm, signature, MUST(String::formatted("{}: Error at property '{}'", where, key)), property)));
        }
        for (auto key : { "allFrames"sv, "matchOriginAsFallback"sv, "persistAcrossSessions"sv }) {
            auto property = TRY(object.get(Utf16FlyString::from_utf8(key)));
            if (property.is_undefined())
                continue;
            if (!property.is_boolean())
                TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Error at property '{}': Invalid type: expected boolean, found {}.", where, key, api_type_name(property)))));
            script.set(key, property.as_bool());
        }
        struct Choice {
            StringView key;
            ReadonlySpan<StringView> values;
            StringView listed;
        };
        static constexpr Array run_at_values { "document_start"sv, "document_end"sv, "document_idle"sv };
        static constexpr Array world_values { "ISOLATED"sv, "MAIN"sv };
        for (auto choice : { Choice { "runAt"sv, run_at_values, "document_start, document_end, document_idle"sv }, Choice { "world"sv, world_values, "ISOLATED, MAIN"sv } }) {
            auto property = TRY(object.get(Utf16FlyString::from_utf8(choice.key)));
            if (property.is_undefined())
                continue;
            auto name = property.is_string() ? property.as_string().utf16_string().to_utf8() : String {};
            if (!choice.values.contains_slow(name.bytes_as_string_view()))
                TRY(throw_invocation_error(vm, signature, MUST(String::formatted("{}: Error at property '{}': Value must be one of {}.", where, choice.key, choice.listed))));
            script.set(choice.key, name);
        }
        scripts.must_append(move(script));
    }
    return scripts;
}

static JS::ThrowCompletionOr<JsonValue> parse_filter(JS::VM& vm, StringView signature, JS::Value value)
{
    if (value.is_nullish())
        return JsonValue {};
    if (!is_plain_object(value))
        TRY(throw_invocation_error(vm, signature, MUST(String::formatted("Error at parameter 'filter': Invalid type: expected scripting.ContentScriptFilter, found {}.", api_type_name(value)))));
    JsonObject filter;
    auto ids = TRY(value.as_object().get("ids"_utf16_fly_string));
    if (!ids.is_undefined())
        filter.set("ids"sv, TRY(parse_strings(vm, signature, "Error at parameter 'filter': Error at property 'ids'"sv, ids)));
    return JsonValue { move(filter) };
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
    struct CSSMethod {
        StringView name;
        StringView signature;
    };
    struct RegistrationMethod {
        StringView name;
        StringView signature;
        bool takes_scripts;
    };
    for (auto method : { RegistrationMethod { "registerContentScripts"sv, register_signature, true }, RegistrationMethod { "updateContentScripts"sv, update_signature, true }, RegistrationMethod { "getRegisteredContentScripts"sv, get_signature, false }, RegistrationMethod { "unregisterContentScripts"sv, unregister_signature, false } }) {
        scripting->define_native_function(realm, Utf16FlyString::from_utf8(method.name), [context = GC::Ref { context }, method](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto count = vm.argument_count();
            GC::Ptr<JS::FunctionObject> callback;
            if (count > 0 && vm.argument(count - 1).is_function())
                callback = vm.argument(--count).as_function();
            JsonArray arguments;
            if (method.takes_scripts)
                arguments.must_append(TRY(parse_registered_scripts(vm, method.signature, vm.argument(0))));
            else
                arguments.must_append(TRY(parse_filter(vm, method.signature, count > 0 ? vm.argument(0) : JS::js_undefined())));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("scripting.{}", method.name)), arguments)); }, method.takes_scripts ? 1 : 0, attributes);
    }
    for (auto [name, signature] : { CSSMethod { "insertCSS"sv, insert_css_signature }, CSSMethod { "removeCSS"sv, remove_css_signature } }) {
        scripting->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, name, signature](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(1).is_function())
                callback = vm.argument(1).as_function();
            JsonArray arguments;
            arguments.must_append(TRY(parse_css_injection(vm, signature, vm.argument(0))));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("scripting.{}", name)), arguments)); }, 1, attributes);
    }
    return scripting;
}

}
