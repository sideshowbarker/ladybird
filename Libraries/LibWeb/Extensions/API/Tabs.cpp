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
#include <LibWeb/Extensions/API/Arguments.h>
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
    return throw_property_error(vm, key, MUST(String::formatted("Invalid type: expected {}, found {}.", expected, api_type_name(value))));
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
        TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Invalid type: expected object, found {}.", api_type_name(argument)))));
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
                TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Error at property 'frameId': Invalid type: expected integer, found {}.", api_type_name(value)))));
            options.frame_id = static_cast<i64>(value.as_double());
        } else if (name == "documentId"sv) {
            if (value.is_undefined())
                continue;
            if (!value.is_string())
                TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Error at property 'documentId': Invalid type: expected string, found {}.", api_type_name(value)))));
            options.has_document_id = true;
        } else {
            TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'options': Unexpected property: '{}'.", name))));
        }
    }
    return options;
}

// One property tabs.create() or tabs.update() knows, and whether Ladybird supports it yet.
struct TabProperty {
    StringView name;
    QueryValueType type;
    bool supported;
};

static constexpr TabProperty create_properties[] = {
    { "active"sv, QueryValueType::Boolean, true },
    { "index"sv, QueryValueType::Integer, true },
    { "openerTabId"sv, QueryValueType::Integer, false },
    { "pinned"sv, QueryValueType::Boolean, true },
    { "selected"sv, QueryValueType::Boolean, false },
    { "url"sv, QueryValueType::String, true },
    { "windowId"sv, QueryValueType::Integer, true },
};

static constexpr TabProperty update_properties[] = {
    { "active"sv, QueryValueType::Boolean, true },
    { "autoDiscardable"sv, QueryValueType::Boolean, false },
    { "highlighted"sv, QueryValueType::Boolean, false },
    { "muted"sv, QueryValueType::Boolean, false },
    { "openerTabId"sv, QueryValueType::Integer, false },
    { "pinned"sv, QueryValueType::Boolean, true },
    { "selected"sv, QueryValueType::Boolean, false },
    { "url"sv, QueryValueType::String, true },
};

// The method a tabs.create() or tabs.update() error names, with the signature it quotes.
struct TabMethod {
    StringView name;
    StringView signature;
    StringView parameter;
    ReadonlySpan<TabProperty> properties;
};

static constexpr TabMethod create_method { "create"sv, "object createProperties, optional function callback"sv, "createProperties"sv, create_properties };
static constexpr TabMethod update_method { "update"sv, "optional integer tabId, object updateProperties, optional function callback"sv, "updateProperties"sv, update_properties };

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_tab_method_error(JS::VM& vm, TabMethod const& method, StringView detail)
{
    return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of tabs.{}({}): {}", method.name, method.signature, detail));
}

struct ParsedTabProperties {
    JsonObject properties;
    Optional<String> unsupported_key;
};

static JS::ThrowCompletionOr<ParsedTabProperties> parse_tab_properties(JS::VM& vm, TabMethod const& method, JS::Value argument)
{
    if (!argument.is_object() || argument.is_function() || is<JS::Array>(argument.as_object())) {
        TRY(throw_tab_method_error(vm, method, MUST(String::formatted("Error at parameter '{}': Invalid type: expected object, found {}.", method.parameter, api_type_name(argument)))));
        VERIFY_NOT_REACHED();
    }
    ParsedTabProperties parsed;
    auto& object = argument.as_object();
    for (auto const& property_key : TRY(object.internal_own_property_keys())) {
        auto key = TRY(JS::PropertyKey::from_value(vm, property_key));
        if (key.is_symbol())
            continue;
        auto descriptor = TRY(object.internal_get_own_property(key));
        if (!descriptor.has_value() || !*descriptor->enumerable)
            continue;
        auto name = key.is_string() ? key.as_string().to_utf16_string().to_utf8() : String::number(key.as_number());
        auto known = find_if(method.properties.begin(), method.properties.end(), [&](auto const& property) { return property.name == name; });
        if (known == method.properties.end())
            TRY(throw_tab_method_error(vm, method, MUST(String::formatted("Error at parameter '{}': Unexpected property: '{}'.", method.parameter, name))));
        auto value = TRY(object.get(key));
        if (value.is_undefined())
            continue;
        auto type_error = [&](StringView expected) {
            return throw_tab_method_error(vm, method, MUST(String::formatted("Error at parameter '{}': Error at property '{}': Invalid type: expected {}, found {}.", method.parameter, name, expected, api_type_name(value))));
        };
        switch (known->type) {
        case QueryValueType::Boolean:
            if (!value.is_boolean())
                TRY(type_error("boolean"sv));
            parsed.properties.set(name, value.as_bool());
            break;
        case QueryValueType::Integer:
            if (!is_api_integer(value))
                TRY(type_error("integer"sv));
            parsed.properties.set(name, static_cast<i64>(value.as_double()));
            break;
        case QueryValueType::String:
            if (!value.is_string())
                TRY(type_error("string"sv));
            parsed.properties.set(name, value.as_string().utf16_string().to_utf8());
            break;
        case QueryValueType::Patterns:
            VERIFY_NOT_REACHED();
        }
        if (!known->supported && !parsed.unsupported_key.has_value())
            parsed.unsupported_key = name;
    }
    return parsed;
}

// Rejects a call that names a property Ladybird doesn't support yet, or else hands it to the UI process.
static JS::Value call_tab_method(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, TabMethod const& method, ParsedTabProperties parsed, Optional<JsonValue> tab_id = {})
{
    auto& realm = context.realm();
    if (parsed.unsupported_key.has_value()) {
        auto message = MUST(String::formatted("tabs.{}() doesn't support the \"{}\" property", method.name, *parsed.unsupported_key));
        return settle_api_call(context, callback, WebIDL::create_rejected_promise(realm, JS::Error::create(realm, Utf16String::from_utf8(message))));
    }
    JsonArray arguments;
    if (tab_id.has_value())
        arguments.must_append(tab_id.release_value());
    arguments.must_append(move(parsed.properties));
    return settle_api_call(context, callback, MessageRouter::the().call_api(context, MUST(String::formatted("tabs.{}", method.name)), arguments));
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
            TRY(throw_send_message_error(vm, MUST(String::formatted("Error at parameter 'tabId': Invalid type: expected integer, found {}.", api_type_name(vm.argument(0))))));
        auto options = TRY(parse_send_message_options(vm, count > 2 ? vm.argument(2) : JS::js_undefined()));
        auto& operation_realm = context->realm();
        // No API hands out document ids yet, so none can name a document.
        if (options.has_document_id)
            return settle_api_call(*context, callback, WebIDL::create_rejected_promise(operation_realm, JS::Error::create(operation_realm, "tabs.sendMessage() doesn't support documentId"_utf16)));
        auto tab_id = static_cast<i64>(vm.argument(0).as_double());
        return settle_api_call(*context, callback, MessageRouter::the().send_to_tab(*context, tab_id, options.frame_id, vm.argument(1))); }, 2, attributes);
    tabs->define_native_function(realm, "create"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto count = vm.argument_count();
        GC::Ptr<JS::FunctionObject> callback;
        if (count > 1 && vm.argument(count - 1).is_function())
            callback = vm.argument(--count).as_function();
        if (count > 1)
            TRY(throw_tab_method_error(vm, create_method, "No matching signature."sv));
        auto parsed = TRY(parse_tab_properties(vm, create_method, vm.argument(0)));
        return call_tab_method(*context, callback, create_method, move(parsed)); }, 1, attributes);
    tabs->define_native_function(realm, "update"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto count = vm.argument_count();
        GC::Ptr<JS::FunctionObject> callback;
        if (count > 1 && vm.argument(count - 1).is_function())
            callback = vm.argument(--count).as_function();
        if (count > 2)
            TRY(throw_tab_method_error(vm, update_method, "No matching signature."sv));
        // The tab id is optional, so a single argument is the properties.
        JsonValue tab_id;
        auto properties = vm.argument(0);
        if (count == 2) {
            if (!vm.argument(0).is_nullish()) {
                if (!is_api_integer(vm.argument(0)))
                    TRY(throw_tab_method_error(vm, update_method, MUST(String::formatted("Error at parameter 'tabId': Invalid type: expected integer, found {}.", api_type_name(vm.argument(0))))));
                tab_id = static_cast<i64>(vm.argument(0).as_double());
            }
            properties = vm.argument(1);
        }
        auto parsed = TRY(parse_tab_properties(vm, update_method, properties));
        return call_tab_method(*context, callback, update_method, move(parsed), move(tab_id)); }, 1, attributes);
    return tabs;
}

}
