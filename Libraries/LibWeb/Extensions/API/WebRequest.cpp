/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/Utf16String.h>
#include <LibGC/RootVector.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/WebRequest.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(WebRequestListeners);

static u64 s_next_listener_id { 0 };

WebRequestListeners::WebRequestListeners(MessagingContext& context)
    : m_context(context)
{
}

void WebRequestListeners::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_context);
    for (auto const& entry : m_entries)
        visitor.visit(entry.callback);
}

void WebRequestListeners::add(JS::FunctionObject& callback, WebRequestListener record, bool reported)
{
    // Adding a function the event already has changes nothing, not even its filter.
    if (has(record.event, callback))
        return;
    record.id = ++s_next_listener_id;
    m_entries.append({ move(record), callback, reported });
    if (reported)
        report();
}

void WebRequestListeners::remove(WebRequestEvent event, JS::FunctionObject const& callback)
{
    bool reported = false;
    m_entries.remove_all_matching([&](auto const& entry) {
        if (entry.record.event != event || entry.callback.ptr() != &callback)
            return false;
        reported = entry.reported;
        return true;
    });
    if (reported)
        report();
}

bool WebRequestListeners::has(WebRequestEvent event, JS::FunctionObject const& callback) const
{
    return any_of(m_entries, [&](auto const& entry) { return entry.record.event == event && entry.callback.ptr() == &callback; });
}

GC::Ptr<JS::FunctionObject> WebRequestListeners::callback_for(u64 listener_id) const
{
    for (auto const& entry : m_entries) {
        if (entry.record.id == listener_id)
            return entry.callback;
    }
    return nullptr;
}

WebRequestListener const* WebRequestListeners::record_for(u64 listener_id) const
{
    for (auto const& entry : m_entries) {
        if (entry.record.id == listener_id)
            return &entry.record;
    }
    return nullptr;
}

void WebRequestListeners::revoke()
{
    m_active = false;
    m_entries.clear();
}

void WebRequestListeners::report()
{
    if (m_context->is_detached())
        return;
    Vector<WebRequestListener> records;
    for (auto const& entry : m_entries) {
        if (entry.reported)
            records.append(entry.record);
    }
    m_context->page().client().page_did_change_extension_web_request_listeners(m_context->extension_id(), m_context->context_id(), move(records));
}

// The extraInfoSpec values each event takes, in the order its enumeration lists them.
static ReadonlySpan<StringView> options_for(WebRequestEvent event)
{
    static constexpr Array before_request { "blocking"sv, "requestBody"sv };
    static constexpr Array before_send_headers { "requestHeaders"sv, "blocking"sv };
    static constexpr Array send_headers { "requestHeaders"sv };
    static constexpr Array headers_received { "blocking"sv, "responseHeaders"sv };
    static constexpr Array auth_required { "responseHeaders"sv, "blocking"sv, "asyncBlocking"sv };
    static constexpr Array response_headers { "responseHeaders"sv };
    switch (event) {
    case WebRequestEvent::BeforeRequest:
        return before_request.span();
    case WebRequestEvent::BeforeSendHeaders:
        return before_send_headers.span();
    case WebRequestEvent::SendHeaders:
        return send_headers.span();
    case WebRequestEvent::HeadersReceived:
        return headers_received.span();
    case WebRequestEvent::AuthRequired:
        return auth_required.span();
    case WebRequestEvent::ResponseStarted:
    case WebRequestEvent::BeforeRedirect:
    case WebRequestEvent::Completed:
        return response_headers.span();
    case WebRequestEvent::ErrorOccurred:
        return {};
    }
    VERIFY_NOT_REACHED();
}

// An enumeration as extensions see it: each value under its name in uppercase, so ResourceType.MAIN_FRAME is
// "main_frame" and OnBeforeRequestOptions.REQUESTBODY is "requestBody".
static GC::Ref<JS::Object> create_enumeration(JS::Realm& realm, ReadonlySpan<StringView> values)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto value : values)
        object->define_direct_property(Utf16FlyString::from_utf8(MUST(String::from_utf8(value)).to_ascii_uppercase()), JS::PrimitiveString::create(realm.vm(), value), JS::default_attributes);
    return object;
}

// Extensions compare these messages, so their wording is fixed.
static JS::Completion throw_listener_error(JS::VM& vm, WebRequestEvent event, StringView message)
{
    return vm.throw_completion<JS::Error>(Utf16String::formatted("{} for webRequest.{}.addListener.", message, web_request_event_name(event)));
}

static JS::Completion throw_parameter_error(JS::VM& vm, WebRequestEvent event, StringView parameter, StringView message)
{
    return throw_listener_error(vm, event, MUST(String::formatted("Type error for parameter {} ({})", parameter, message)));
}

static JS::ThrowCompletionOr<String> stringify(JS::VM& vm, JS::Value value)
{
    auto json = TRY(JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::js_undefined()));
    return json.has_value() ? json->to_utf8() : "undefined"_string;
}

static bool is_plain_object(JS::Value value)
{
    return value.is_object() && !value.is_function() && !is<JS::Array>(value.as_object());
}

static bool is_array(JS::Value value)
{
    return value.is_object() && is<JS::Array>(value.as_object());
}

static JS::ThrowCompletionOr<GC::RootVector<JS::Value>> array_items(JS::VM& vm, JS::Value array)
{
    GC::RootVector<JS::Value> items;
    auto length = TRY(JS::length_of_array_like(vm, array.as_object()));
    for (size_t i = 0; i < length; ++i)
        items.append(TRY(array.as_object().get(i)));
    return items;
}

// The filter's urls, types, tabId, and windowId, checked in that order, with the message of the first problem.
struct ParsedFilter {
    Vector<String> urls;
    Vector<String> types;
    Optional<i64> tab_id;
    Optional<i64> window_id;
};

static JS::ThrowCompletionOr<Optional<String>> parse_filter(JS::VM& vm, JS::Realm& realm, JS::Object& object, ParsedFilter& filter)
{
    using Problem = Optional<String>;
    auto expected = [&](StringView path, StringView type, JS::Value value) -> JS::ThrowCompletionOr<Problem> {
        return Problem { MUST(String::formatted("Error processing {}: Expected {} instead of {}", path, type, TRY(stringify(vm, value)))) };
    };

    auto urls = TRY(object.get("urls"_utf16_fly_string));
    if (urls.is_undefined())
        return Problem { "Property \"urls\" is required"_string };
    if (!is_array(urls))
        return expected("urls"sv, "array"sv, urls);
    auto url_items = TRY(array_items(vm, urls));
    for (size_t i = 0; i < url_items.size(); ++i) {
        if (!url_items[i].is_string())
            return expected(MUST(String::formatted("urls.{}", i)), "string"sv, url_items[i]);
        filter.urls.append(url_items[i].as_string().utf16_string().to_utf8());
    }
    if (filter.urls.is_empty())
        return Problem { "Error processing urls: Array requires at least 1 items; you have 0"_string };

    if (auto types = TRY(object.get("types"_utf16_fly_string)); !types.is_undefined()) {
        if (!is_array(types))
            return expected("types"sv, "array"sv, types);
        // A type this browser doesn't know is dropped with a warning, so a filter written for a newer one still works.
        for (auto [i, item] : enumerate(TRY(array_items(vm, types)))) {
            auto name = item.is_string() ? item.as_string().utf16_string().to_utf8() : String {};
            if (!item.is_string() || !web_request_resource_types().contains_slow(name)) {
                auto& console = realm.intrinsics().console_object()->console();
                console.output_debug_message(JS::Console::LogLevel::Warn, Utf16String::formatted("Ignoring the webRequest filter type at types.{}: {}", i, TRY(stringify(vm, item))));
                continue;
            }
            filter.types.append(move(name));
        }
        if (filter.types.is_empty())
            return Problem { "Error processing types: Array requires at least 1 items; you have 0"_string };
    }

    struct IntegerProperty {
        StringView name;
        Optional<i64>& target;
    };
    for (auto [name, target] : Array<IntegerProperty, 2> { { { "tabId"sv, filter.tab_id }, { "windowId"sv, filter.window_id } } }) {
        auto value = TRY(object.get(Utf16FlyString::from_utf8(name)));
        if (value.is_undefined())
            continue;
        if (!value.is_integral_number())
            return expected(name, "integer"sv, value);
        target = static_cast<i64>(value.as_double());
    }

    if (auto incognito = TRY(object.get("incognito"_utf16_fly_string)); !incognito.is_undefined() && !incognito.is_boolean())
        return expected("incognito"sv, "boolean"sv, incognito);

    for (auto const& key : TRY(object.internal_own_property_keys())) {
        auto property_key = TRY(JS::PropertyKey::from_value(vm, key));
        if (property_key.is_symbol())
            continue;
        auto name = property_key.is_string() ? property_key.as_string().to_utf16_string().to_utf8() : String::number(property_key.as_number());
        if (!name.is_one_of("urls"sv, "types"sv, "tabId"sv, "windowId"sv, "incognito"sv))
            return Problem { MUST(String::formatted("Unexpected property \"{}\"", name)) };
    }
    return Problem {};
}

static JS::ThrowCompletionOr<JS::Value> add_listener(JS::VM& vm, MessagingContext& context, WebRequestListeners& listeners, WebRequestEvent event)
{
    if (!listeners.is_active() || context.is_detached())
        return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);

    auto options = options_for(event);
    bool takes_options = event != WebRequestEvent::ErrorOccurred;
    auto callback = vm.argument(0);
    auto filter_value = vm.argument(1);
    auto options_value = vm.argument(2);
    auto has_options = !options_value.is_nullish();
    // A trailing undefined argument counts as omitted.
    auto argument_count = vm.argument_count();
    while (argument_count > 0 && vm.argument(argument_count - 1).is_undefined())
        --argument_count;
    auto too_many = argument_count > (takes_options ? 3u : 2u);
    if (!callback.is_function() || !is_plain_object(filter_value) || (has_options && (!takes_options || !is_array(options_value))) || too_many)
        return throw_listener_error(vm, event, "Incorrect argument types"sv);

    ParsedFilter filter;
    if (auto problem = TRY(parse_filter(vm, *vm.current_realm(), filter_value.as_object(), filter)); problem.has_value())
        return throw_parameter_error(vm, event, "filter"sv, *problem);

    WebRequestListener record;
    record.event = event;
    record.types = move(filter.types);
    record.tab_id = filter.tab_id;
    record.window_id = filter.window_id;

    if (has_options) {
        for (auto [i, item] : enumerate(TRY(array_items(vm, options_value)))) {
            if (!item.is_string())
                return throw_parameter_error(vm, event, "extraInfoSpec"sv, MUST(String::formatted("Error processing {}: Expected string instead of {}", i, TRY(stringify(vm, item)))));
            auto option = item.as_string().utf16_string().to_utf8();
            // Extensions that don't recognize this browser pass "extraHeaders" to see every header; every listener
            // here sees them all already, so it's accepted and changes nothing.
            if (option == "extraHeaders"sv)
                continue;
            if (!options.contains_slow(option))
                return throw_parameter_error(vm, event, "extraInfoSpec"sv, MUST(String::formatted("Error processing {}: Invalid enumeration value \"{}\"", i, option)));
            if (option == "blocking"sv) {
                if (event == WebRequestEvent::AuthRequired && !context.descriptor().has_permission("webRequestBlocking"sv))
                    return vm.throw_completion<JS::Error>("Using webRequest.onAuthRequired.addListener with the blocking option requires either the 'webRequestBlocking' or 'webRequestAuthProvider' permission."sv);
                if (!context.descriptor().has_permission("webRequestBlocking"sv))
                    return vm.throw_completion<JS::Error>("Using webRequest.addListener with the blocking option requires the 'webRequestBlocking' permission."sv);
                record.blocking = true;
            } else if (option == "asyncBlocking"sv) {
                record.blocking = true;
            } else if (option == "requestHeaders"sv) {
                record.request_headers = true;
            } else if (option == "responseHeaders"sv) {
                record.response_headers = true;
            }
        }
    }

    // A pattern that doesn't parse is reported to the console rather than thrown, and the listener never hears
    // anything; an extension may name its own pages, but no other extension's.
    bool valid = true;
    for (auto const& url : filter.urls) {
        auto allow_extension_scheme = url.starts_with_bytes(MUST(String::formatted("{}://{}/", scheme, context.extension_id())))
            ? MatchPattern::AllowExtensionScheme::Yes
            : MatchPattern::AllowExtensionScheme::No;
        auto pattern = MatchPattern::parse(url, allow_extension_scheme);
        if (pattern.is_error()) {
            auto& console = vm.current_realm()->intrinsics().console_object()->console();
            console.output_debug_message(JS::Console::LogLevel::Error, Utf16String::formatted("Invalid url pattern: {}", url));
            valid = false;
            break;
        }
        record.urls.append(pattern.release_value());
    }

    listeners.add(callback.as_function(), move(record), valid);
    return JS::js_undefined();
}

static GC::Ref<JS::Object> create_web_request_event(JS::Realm& realm, MessagingContext& context, WebRequestListeners& listeners, WebRequestEvent event)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    object->define_native_function(realm, "addListener"_utf16_fly_string, [context = GC::Ref { context }, listeners = GC::Ref { listeners }, event](JS::VM& vm) { return add_listener(vm, *context, *listeners, event); }, 1, attributes);
    for (auto name : { "removeListener"sv, "hasListener"sv }) {
        object->define_native_function(realm, Utf16FlyString::from_utf8(name), [listeners = GC::Ref { listeners }, event, name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (!listeners->is_active())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto callback = vm.argument(0);
            if (!callback.is_function() || vm.argument_count() > 1)
                return vm.throw_completion<JS::Error>(Utf16String::formatted("Incorrect argument types for webRequest.{}.{}.", web_request_event_name(event), name));
            if (name == "hasListener"sv)
                return JS::Value { listeners->has(event, callback.as_function()) };
            listeners->remove(event, callback.as_function());
            return JS::js_undefined(); }, 1, attributes);
    }
    return object;
}

GC::Ref<JS::Object> create_web_request(JS::Realm& realm, MessagingContext& context)
{
    auto web_request = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    auto listeners = realm.heap().allocate<WebRequestListeners>(context);
    context.set_web_request_listeners(listeners);

    for (u8 i = 0; i <= to_underlying(WebRequestEvent::ErrorOccurred); ++i) {
        auto event = static_cast<WebRequestEvent>(i);
        web_request->define_direct_property(Utf16FlyString::from_utf8(web_request_event_name(event)), create_web_request_event(realm, context, listeners, event), attributes);
    }

    web_request->define_direct_property("ResourceType"_utf16_fly_string, create_enumeration(realm, web_request_resource_types()), attributes);
    struct OptionEnumeration {
        StringView name;
        WebRequestEvent event;
    };
    static constexpr Array<OptionEnumeration, 8> option_enumerations { {
        { "OnBeforeRequestOptions"sv, WebRequestEvent::BeforeRequest },
        { "OnBeforeSendHeadersOptions"sv, WebRequestEvent::BeforeSendHeaders },
        { "OnSendHeadersOptions"sv, WebRequestEvent::SendHeaders },
        { "OnHeadersReceivedOptions"sv, WebRequestEvent::HeadersReceived },
        { "OnAuthRequiredOptions"sv, WebRequestEvent::AuthRequired },
        { "OnResponseStartedOptions"sv, WebRequestEvent::ResponseStarted },
        { "OnBeforeRedirectOptions"sv, WebRequestEvent::BeforeRedirect },
        { "OnCompletedOptions"sv, WebRequestEvent::Completed },
    } };
    for (auto const& [name, event] : option_enumerations)
        web_request->define_direct_property(Utf16FlyString::from_utf8(name), create_enumeration(realm, options_for(event)), attributes);

    web_request->define_direct_property("MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES"_utf16_fly_string, JS::Value { 20 }, attributes);

    // No request's answer is cached anywhere a listener change could make stale, so there's nothing to flush.
    web_request->define_native_function(realm, "handlerBehaviorChanged"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(0).is_function())
            callback = vm.argument(0).as_function();
        return settle_api_call(*context, callback, WebIDL::create_resolved_promise(*vm.current_realm(), JS::js_undefined())); }, 0, attributes);

    return web_request;
}

}
