/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/ContentScripts.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

// Extensions compare these messages, so their wording is fixed: A check that fails throws an Error at once, naming the
// property it was processing.
static JS::ThrowCompletionOr<void> throw_options_error(JS::VM& vm, StringView detail)
{
    return vm.throw_completion<JS::Error>(Utf16String::formatted("Type error for parameter contentScriptOptions ({}) for contentScripts.register.", detail));
}

// A value as an error shows it: a string quoted, as JSON would.
static String serialized_for_error(JS::Value value)
{
    if (value.is_string())
        return JsonValue { value.as_string().utf16_string().to_utf8() }.serialized();
    return value.to_utf16_string_without_side_effects().to_utf8();
}

static JS::ThrowCompletionOr<void> throw_property_error(JS::VM& vm, StringView path, StringView detail)
{
    return throw_options_error(vm, MUST(String::formatted("Error processing {}: {}", path, detail)));
}

static JS::ThrowCompletionOr<JS::Object*> as_array(JS::VM& vm, StringView path, JS::Value value)
{
    if (!value.is_object() || !is<JS::Array>(value.as_object()))
        TRY(throw_property_error(vm, path, MUST(String::formatted("Expected array instead of {}", serialized_for_error(value)))));
    return &value.as_object();
}

// matches and excludeMatches: arrays of match patterns, with at least one.
static JS::ThrowCompletionOr<Optional<JsonArray>> parse_patterns(JS::VM& vm, JS::Object& options, StringView key, bool required)
{
    auto value = TRY(options.get(Utf16FlyString::from_utf8(key)));
    if (value.is_undefined()) {
        if (required)
            TRY(throw_options_error(vm, MUST(String::formatted("Property \"{}\" is required", key))));
        return Optional<JsonArray> {};
    }
    auto* array = TRY(as_array(vm, key, value));
    auto length = TRY(JS::length_of_array_like(vm, *array));
    if (length == 0)
        TRY(throw_property_error(vm, key, "Array requires at least 1 items; you have 0"sv));
    JsonArray patterns;
    for (size_t i = 0; i < length; ++i) {
        auto item = TRY(array->get(JS::PropertyKey { i }));
        auto path = MUST(String::formatted("{}.{}", key, i));
        if (!item.is_string() || MatchPattern::parse(item.as_string().utf16_string().to_utf8()).is_error())
            TRY(throw_property_error(vm, path, MUST(String::formatted("Value {} must either: be one of [\"<all_urls>\"], or match the format \"matchPattern\"", serialized_for_error(item)))));
        patterns.must_append(item.as_string().utf16_string().to_utf8());
    }
    return patterns;
}

static JS::ThrowCompletionOr<Optional<JsonArray>> parse_strings(JS::VM& vm, JS::Object& options, StringView key)
{
    auto value = TRY(options.get(Utf16FlyString::from_utf8(key)));
    if (value.is_undefined())
        return Optional<JsonArray> {};
    auto* array = TRY(as_array(vm, key, value));
    auto length = TRY(JS::length_of_array_like(vm, *array));
    JsonArray strings;
    for (size_t i = 0; i < length; ++i) {
        auto item = TRY(array->get(JS::PropertyKey { i }));
        if (!item.is_string())
            TRY(throw_property_error(vm, MUST(String::formatted("{}.{}", key, i)), MUST(String::formatted("Expected string instead of {}", serialized_for_error(item)))));
        strings.must_append(item.as_string().utf16_string().to_utf8());
    }
    return strings;
}

// js and css: arrays whose entries each name a file of the extension's or carry code, but not both.
static JS::ThrowCompletionOr<Optional<JsonArray>> parse_sources(JS::VM& vm, JS::Object& options, StringView key)
{
    auto value = TRY(options.get(Utf16FlyString::from_utf8(key)));
    if (value.is_undefined())
        return Optional<JsonArray> {};
    auto* array = TRY(as_array(vm, key, value));
    auto length = TRY(JS::length_of_array_like(vm, *array));
    JsonArray sources;
    for (size_t i = 0; i < length; ++i) {
        auto item = TRY(array->get(JS::PropertyKey { i }));
        auto path = MUST(String::formatted("{}.{}", key, i));
        auto fail = [&] { return throw_property_error(vm, path, "Value must either: .file must be a string, or .code must be a string"sv); };
        if (!item.is_object() || item.is_function() || is<JS::Array>(item.as_object()))
            TRY(fail());
        auto file = TRY(item.as_object().get("file"_utf16_fly_string));
        auto code = TRY(item.as_object().get("code"_utf16_fly_string));
        if (file.is_undefined() == code.is_undefined())
            TRY(fail());
        auto const& source = file.is_undefined() ? code : file;
        if (!source.is_string())
            TRY(fail());
        JsonObject entry;
        entry.set(file.is_undefined() ? "code"sv : "file"sv, source.as_string().utf16_string().to_utf8());
        sources.must_append(move(entry));
    }
    return sources;
}

static JS::ThrowCompletionOr<JsonObject> parse_options(JS::VM& vm, JS::Value value)
{
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object()))
        TRY(throw_options_error(vm, MUST(String::formatted("Expected object instead of {}", serialized_for_error(value)))));
    auto& options = value.as_object();
    JsonObject parsed;
    auto set = [&](StringView key, Optional<JsonArray> array) {
        if (array.has_value())
            parsed.set(key, array.release_value());
    };
    set("matches"sv, TRY(parse_patterns(vm, options, "matches"sv, true)));
    set("excludeMatches"sv, TRY(parse_patterns(vm, options, "excludeMatches"sv, false)));
    set("includeGlobs"sv, TRY(parse_strings(vm, options, "includeGlobs"sv)));
    set("excludeGlobs"sv, TRY(parse_strings(vm, options, "excludeGlobs"sv)));
    set("css"sv, TRY(parse_sources(vm, options, "css"sv)));
    set("js"sv, TRY(parse_sources(vm, options, "js"sv)));
    for (auto key : { "allFrames"sv, "matchAboutBlank"sv, "matchOriginAsFallback"sv }) {
        auto property = TRY(options.get(Utf16FlyString::from_utf8(key)));
        if (property.is_undefined())
            continue;
        if (!property.is_boolean())
            TRY(throw_property_error(vm, key, MUST(String::formatted("Expected boolean instead of {}", serialized_for_error(property)))));
        parsed.set(key, property.as_bool());
    }
    struct Choice {
        StringView key;
        ReadonlySpan<StringView> values;
    };
    static constexpr Array run_at_values { "document_start"sv, "document_end"sv, "document_idle"sv };
    static constexpr Array world_values { "ISOLATED"sv, "MAIN"sv };
    static constexpr Array css_origin_values { "author"sv, "user"sv };
    for (auto choice : { Choice { "runAt"sv, run_at_values }, Choice { "world"sv, world_values }, Choice { "cssOrigin"sv, css_origin_values } }) {
        auto property = TRY(options.get(Utf16FlyString::from_utf8(choice.key)));
        if (property.is_undefined())
            continue;
        auto name = property.is_string() ? property.as_string().utf16_string().to_utf8() : String {};
        if (!property.is_string() || !choice.values.contains_slow(name.bytes_as_string_view()))
            TRY(throw_property_error(vm, choice.key, MUST(String::formatted("Invalid enumeration value {}", serialized_for_error(property)))));
        parsed.set(choice.key, name);
    }
    return parsed;
}

// A registration's handle, whose unregister() takes its script away. Unregistering again resolves and changes nothing.
static GC::Ref<JS::Object> create_registered_content_script(JS::Realm& realm, MessagingContext& context, double handle)
{
    auto registered = JS::Object::create(realm, realm.intrinsics().object_prototype());
    registered->define_native_function(
        realm, "unregister"_utf16_fly_string, GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, handle), [](auto& context, auto& handle, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            JsonArray arguments;
            arguments.must_append(handle);
            return settle_api_call(*context, nullptr, MessageRouter::the().call_api(*context, "contentScripts.unregister"sv, arguments));
        }),
        0, JS::Attribute::Writable | JS::Attribute::Configurable);
    return registered;
}

GC::Ref<JS::Object> create_content_scripts(JS::Realm& realm, MessagingContext& context)
{
    auto content_scripts = JS::Object::create(realm, realm.intrinsics().object_prototype());
    content_scripts->define_native_function(
        realm, "register"_utf16_fly_string, GC::create_function(realm.heap(), GC::captures(GC::Ref { context }), [](auto& context, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            if (vm.argument_count() != 1)
                return vm.throw_completion<JS::Error>(u"Incorrect argument types for contentScripts.register."sv);
            JsonArray arguments;
            arguments.must_append(TRY(parse_options(vm, vm.argument(0))));
            auto registered = WebIDL::upon_fulfillment(MessageRouter::the().call_api(*context, "contentScripts.register"sv, arguments),
                GC::create_function(vm.heap(), GC::captures(context), [](auto& context, JS::Value handle) -> WebIDL::ExceptionOr<JS::Value> {
                    return create_registered_content_script(context->realm(), *context, handle.is_number() ? handle.as_double() : 0);
                }));
            return settle_api_call(*context, nullptr, registered);
        }),
        1, JS::Attribute::Writable | JS::Attribute::Configurable);
    return content_scripts;
}

}
