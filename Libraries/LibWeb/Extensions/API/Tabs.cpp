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
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Tabs.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

enum class QueryValueType : u8 {
    Boolean,
    Integer,
    String,
    Patterns,
};

struct QueryKey {
    StringView name;
    QueryValueType type;
    ReadonlySpan<StringView> allowed_values {};
    bool supported { true };
};

static constexpr Array status_values { "complete"sv, "loading"sv, "unloaded"sv };
static constexpr Array window_type_values { "app"sv, "devtools"sv, "normal"sv, "panel"sv, "popup"sv };

static constexpr Array query_keys {
    QueryKey { "active"sv, QueryValueType::Boolean },
    QueryKey { "audible"sv, QueryValueType::Boolean, {}, false },
    QueryKey { "autoDiscardable"sv, QueryValueType::Boolean, {}, false },
    QueryKey { "currentWindow"sv, QueryValueType::Boolean },
    QueryKey { "discarded"sv, QueryValueType::Boolean },
    QueryKey { "frozen"sv, QueryValueType::Boolean, {}, false },
    QueryKey { "groupId"sv, QueryValueType::Integer },
    QueryKey { "highlighted"sv, QueryValueType::Boolean },
    QueryKey { "index"sv, QueryValueType::Integer },
    QueryKey { "lastFocusedWindow"sv, QueryValueType::Boolean },
    QueryKey { "muted"sv, QueryValueType::Boolean, {}, false },
    QueryKey { "pinned"sv, QueryValueType::Boolean },
    QueryKey { "splitViewId"sv, QueryValueType::Integer, {}, false },
    QueryKey { "status"sv, QueryValueType::String, status_values },
    QueryKey { "title"sv, QueryValueType::String },
    QueryKey { "url"sv, QueryValueType::Patterns },
    QueryKey { "windowId"sv, QueryValueType::Integer },
    QueryKey { "windowType"sv, QueryValueType::String, window_type_values },
};

// An API integer is a signed 32-bit one, and any other number is just a number.
static bool is_api_integer(JS::Value value)
{
    return value.is_integral_number() && value.as_double() >= NumericLimits<i32>::min() && value.as_double() <= NumericLimits<i32>::max();
}

static StringView type_name_of(JS::Value value)
{
    if (value.is_undefined())
        return "undefined"sv;
    if (value.is_null())
        return "null"sv;
    if (value.is_boolean())
        return "boolean"sv;
    if (value.is_number())
        return is_api_integer(value) ? "integer"sv : "number"sv;
    if (value.is_string())
        return "string"sv;
    if (value.is_function())
        return "function"sv;
    if (value.is_object() && is<JS::Array>(value.as_object()))
        return "array"sv;
    return "object"sv;
}

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_invocation_error(JS::VM& vm, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of tabs.query(object queryInfo, optional function callback): {}", detail));
}

static JS::ThrowCompletionOr<void> throw_property_error(JS::VM& vm, StringView key, StringView error)
{
    return throw_invocation_error(vm, MUST(String::formatted("Error at parameter 'queryInfo': Error at property '{}': {}", key, error)));
}

static JS::ThrowCompletionOr<void> throw_type_error(JS::VM& vm, StringView key, StringView expected, JS::Value value)
{
    return throw_property_error(vm, key, MUST(String::formatted("Invalid type: expected {}, found {}.", expected, type_name_of(value))));
}

struct ParsedQuery {
    JsonObject query;
    Optional<String> unsupported_key;
};

// Checks the query's keys and the types of their values the way the API schema does, so a malformed query throws before
// anything is sent.
static JS::ThrowCompletionOr<ParsedQuery> parse_query(JS::VM& vm, JS::Value argument)
{
    ParsedQuery parsed;
    if (!argument.is_object() || argument.is_function() || is<JS::Array>(argument.as_object())) {
        TRY(throw_invocation_error(vm, "No matching signature."sv));
        VERIFY_NOT_REACHED();
    }
    auto& object = argument.as_object();
    for (auto const& property_key : TRY(object.internal_own_property_keys())) {
        auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
        if (key.is_symbol())
            continue;
        auto descriptor = TRY(object.internal_get_own_property(key));
        if (!descriptor.has_value() || !*descriptor->enumerable)
            continue;
        auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
        auto value = TRY(object.get(key));
        auto known = AK::find_if(query_keys.begin(), query_keys.end(), [&](auto const& candidate) { return candidate.name == name.bytes_as_string_view(); });
        if (known == query_keys.end()) {
            TRY(throw_invocation_error(vm, MUST(String::formatted("Error at parameter 'queryInfo': Unexpected property: '{}'.", name))));
            VERIFY_NOT_REACHED();
        }
        // An undefined value stands for a key that isn't there.
        if (value.is_undefined())
            continue;
        switch (known->type) {
        case QueryValueType::Boolean:
            if (!value.is_boolean())
                TRY(throw_type_error(vm, name, "boolean"sv, value));
            parsed.query.set(name, value.as_bool());
            break;
        case QueryValueType::Integer:
            if (!is_api_integer(value))
                TRY(throw_type_error(vm, name, "integer"sv, value));
            parsed.query.set(name, static_cast<i64>(value.as_double()));
            break;
        case QueryValueType::String: {
            if (!value.is_string())
                TRY(throw_type_error(vm, name, "string"sv, value));
            auto string = value.as_string().utf16_string().to_utf8();
            if (!known->allowed_values.is_empty() && !known->allowed_values.contains_slow(string.bytes_as_string_view())) {
                StringBuilder allowed;
                allowed.join(", "sv, known->allowed_values);
                TRY(throw_property_error(vm, name, MUST(String::formatted("Value must be one of {}.", allowed.string_view()))));
            }
            parsed.query.set(name, string);
            break;
        }
        case QueryValueType::Patterns: {
            if (value.is_string()) {
                parsed.query.set(name, value.as_string().utf16_string().to_utf8());
                break;
            }
            if (!value.is_object() || !is<JS::Array>(value.as_object()))
                TRY(throw_property_error(vm, name, "Value did not match any choice."sv));
            JsonArray patterns;
            auto length = TRY(JS::length_of_array_like(vm, value.as_object()));
            for (size_t index = 0; index < length; ++index) {
                auto item = TRY(value.as_object().get(index));
                if (!item.is_string())
                    TRY(throw_property_error(vm, name, "Value did not match any choice."sv));
                patterns.must_append(item.as_string().utf16_string().to_utf8());
            }
            parsed.query.set(name, move(patterns));
            break;
        }
        }
        if (!known->supported && !parsed.unsupported_key.has_value())
            parsed.unsupported_key = name;
    }
    return parsed;
}

static JS::ThrowCompletionOr<void> throw_send_message_error(JS::VM& vm, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of tabs.sendMessage(integer tabId, any message, optional object options, optional function callback): {}", detail));
}

struct SendMessageOptions {
    Optional<i64> frame_id;
    bool has_document_id { false };
};

static JS::ThrowCompletionOr<SendMessageOptions> parse_send_message_options(JS::VM& vm, JS::Value argument)
{
    SendMessageOptions options;
    if (argument.is_nullish())
        return options;
    if (!argument.is_object() || argument.is_function() || is<JS::Array>(argument.as_object())) {
        TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Invalid type: expected object, found {}.", type_name_of(argument)))));
        VERIFY_NOT_REACHED();
    }
    auto& object = argument.as_object();
    for (auto const& property_key : TRY(object.internal_own_property_keys())) {
        auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
        if (key.is_symbol())
            continue;
        auto descriptor = TRY(object.internal_get_own_property(key));
        if (!descriptor.has_value() || !*descriptor->enumerable)
            continue;
        auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
        auto value = TRY(object.get(key));
        if (name == "frameId"sv) {
            if (value.is_undefined())
                continue;
            if (!is_api_integer(value))
                TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Error at property 'frameId': Invalid type: expected integer, found {}.", type_name_of(value)))));
            options.frame_id = static_cast<i64>(value.as_double());
        } else if (name == "documentId"sv) {
            if (value.is_undefined())
                continue;
            if (!value.is_string())
                TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Error at property 'documentId': Invalid type: expected string, found {}.", type_name_of(value)))));
            options.has_document_id = true;
        } else {
            TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Unexpected property: '{}'.", name))));
        }
    }
    return options;
}

GC::Ref<JS::Object> create_tabs(JS::Realm& realm, MessagingContext& context)
{
    auto tabs = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    tabs->define_direct_property("TAB_ID_NONE"_utf16_fly_string, JS::Value(-1), 0);
    tabs->define_direct_property("TAB_INDEX_NONE"_utf16_fly_string, JS::Value(-1), 0);
    tabs->define_native_function(realm, "query"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(1).is_function())
            callback = vm.argument(1).as_function();
        auto parsed = TRY(parse_query(vm, vm.argument(0)));
        auto& operation_realm = context->realm();
        if (parsed.unsupported_key.has_value()) {
            auto message = MUST(String::formatted("tabs.query() doesn't support the \"{}\" filter", *parsed.unsupported_key));
            return settle_api_call(*context, callback, WebIDL::create_rejected_promise(operation_realm, JS::Error::create(operation_realm, Utf16String::from_utf8(message))));
        }
        return settle_api_call(*context, callback, MessageRouter::the().query_tabs(*context, parsed.query.serialized())); }, 1, attributes);
    tabs->define_native_function(realm, "sendMessage"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto count = vm.argument_count();
        GC::Ptr<JS::FunctionObject> callback;
        if (count > 2 && vm.argument(count - 1).is_function())
            callback = vm.argument(--count).as_function();
        if (count > 3)
            TRY(throw_send_message_error(vm, "No matching signature."sv));
        if (!is_api_integer(vm.argument(0)))
            TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'tabId': Invalid type: expected integer, found {}.", type_name_of(vm.argument(0))))));
        auto options = TRY(parse_send_message_options(vm, count > 2 ? vm.argument(2) : JS::js_undefined()));
        auto& operation_realm = context->realm();
        // No API hands out document ids yet, so none can name a document.
        if (options.has_document_id)
            return settle_api_call(*context, callback, WebIDL::create_rejected_promise(operation_realm, JS::Error::create(operation_realm, "tabs.sendMessage() doesn't support documentId"_utf16)));
        auto tab_id = static_cast<i64>(vm.argument(0).as_double());
        return settle_api_call(*context, callback, MessageRouter::the().send_to_tab(*context, tab_id, options.frame_id, vm.argument(1))); }, 2, attributes);
    return tabs;
}

}
