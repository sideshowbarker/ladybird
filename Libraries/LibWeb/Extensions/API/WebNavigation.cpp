/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/RefCounted.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/Parser.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/WebNavigation.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWebCommon/Extensions/UrlFilter.h>

namespace Web::Extensions {

// The URL filters one listener gave; with none, it hears every URL.
struct WebNavigationFilters : public RefCounted<WebNavigationFilters> {
    Vector<UrlFilter> filters;
};

// A listener of one event and the function the event's list calls in its place, which hands on only the details whose
// URL one of the listener's filters matches.
class WebNavigationListeners final : public GC::Cell {
    GC_CELL(WebNavigationListeners, GC::Cell);
    GC_DECLARE_ALLOCATOR(WebNavigationListeners);

public:
    struct Entry {
        GC::Ref<JS::FunctionObject> callback;
        GC::Ref<JS::FunctionObject> wrapper;
    };
    HashMap<String, Vector<Entry>>& entries() { return m_entries; }

private:
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        for (auto const& [name, entries] : m_entries) {
            for (auto const& entry : entries) {
                visitor.visit(entry.callback);
                visitor.visit(entry.wrapper);
            }
        }
    }

    HashMap<String, Vector<Entry>> m_entries;
};

GC_DEFINE_ALLOCATOR(WebNavigationListeners);

// Extensions compare these messages, so their wording is fixed.
static JS::Completion throw_listener_error(JS::VM& vm, StringView event, StringView message)
{
    return vm.throw_completion<JS::Error>(Utf16String::formatted("{} for webNavigation.{}.addListener.", message, event));
}

// A filter's own error, placed at the filter's index in the url list.
static String nested_filter_error(size_t index, String const& error)
{
    static constexpr auto prefix = "Error processing "sv;
    if (error.starts_with_bytes(prefix))
        return MUST(String::formatted("Error processing url.{}.{}", index, error.bytes_as_string_view().substring_view(prefix.length())));
    return MUST(String::formatted("Error processing url.{}: {}", index, error));
}

static JS::ThrowCompletionOr<NonnullRefPtr<WebNavigationFilters>> parse_filters(JS::VM& vm, StringView event, JS::Value value)
{
    auto filters = adopt_ref(*new WebNavigationFilters);
    if (value.is_undefined())
        return filters;
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object()))
        return throw_listener_error(vm, event, "Incorrect argument types"sv);
    auto json = TRY(JS::JSONObject::stringify_impl(vm, value, JS::js_undefined(), JS::js_undefined()));
    auto parsed = json.has_value() ? JsonValue::from_string(json->to_utf8()) : ErrorOr<JsonValue> { Error::from_string_literal("") };
    if (parsed.is_error() || !parsed.value().is_object())
        return throw_listener_error(vm, event, "Incorrect argument types"sv);
    auto const& object = parsed.value().as_object();
    auto parameter_error = [&](String const& message) {
        return throw_listener_error(vm, event, MUST(String::formatted("Type error for parameter filters ({})", message)));
    };
    Optional<String> unexpected;
    object.for_each_member([&](String const& key, JsonValue const&) {
        if (key != "url"sv && !unexpected.has_value())
            unexpected = key;
    });
    if (unexpected.has_value())
        return parameter_error(MUST(String::formatted("Unexpected property \"{}\"", *unexpected)));
    auto url = object.get("url"sv);
    if (!url.has_value())
        return parameter_error("Property \"url\" is required"_string);
    if (!url->is_array())
        return parameter_error(MUST(String::formatted("Error processing url: Expected array instead of {}", url->serialized())));
    auto const& entries = url->as_array().values();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].is_object())
            return parameter_error(MUST(String::formatted("Error processing url.{}: Expected object instead of {}", i, entries[i].serialized())));
        auto filter = UrlFilter::parse(entries[i].as_object());
        if (filter.is_error())
            return parameter_error(nested_filter_error(i, filter.error()));
        filters->filters.append(filter.release_value());
    }
    if (filters->filters.is_empty())
        return parameter_error("Error processing url: Array requires at least 1 items; you have 0"_string);
    return filters;
}

static bool details_match(JS::VM& vm, WebNavigationFilters const& filters, JS::Value details)
{
    if (filters.filters.is_empty())
        return true;
    if (!details.is_object())
        return false;
    auto url_value = details.as_object().get("url"_utf16_fly_string);
    if (url_value.is_error() || !url_value.value().is_string())
        return false;
    (void)vm;
    auto url = URL::Parser::basic_parse(url_value.value().as_string().utf16_string().to_utf8());
    return url.has_value() && any_of(filters.filters, [&](auto const& filter) { return filter.matches(*url); });
}

static GC::Ref<JS::Object> create_web_navigation_event(JS::Realm& realm, MessagingContext& context, WebNavigationListeners& listeners, StringView event)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto event_name = MUST(String::from_utf8(event));
    auto list_name = MUST(String::formatted("webNavigation.{}", event));

    object->define_native_function(realm, "addListener"_utf16_fly_string, [context = GC::Ref { context }, listeners = GC::Ref { listeners }, event_name, list_name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto callback_value = vm.argument(0);
        if (!callback_value.is_function())
            return throw_listener_error(vm, event_name, "Incorrect argument types"sv);
        auto& callback = callback_value.as_function();
        auto& entries = listeners->entries().ensure(event_name);
        if (any_of(entries, [&](auto const& entry) { return entry.callback.ptr() == &callback; }))
            return JS::js_undefined();
        auto filters = TRY(parse_filters(vm, event_name, vm.argument(1)));
        auto& realm = *vm.current_realm();
        auto wrapper = JS::NativeFunction::create(realm, [callback = GC::Ref { callback }, filters](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto details = vm.argument(0);
            if (!details_match(vm, *filters, details))
                return JS::js_undefined();
            return JS::call(vm, JS::Value { callback }, JS::js_undefined(), details); }, 1, Utf16FlyString {});
        entries.append({ callback, wrapper });
        context->event_listeners(list_name).add(wrapper);
        return JS::js_undefined(); }, 1, attributes);

    for (auto name : { "removeListener"sv, "hasListener"sv }) {
        object->define_native_function(realm, Utf16FlyString::from_utf8(name), [context = GC::Ref { context }, listeners = GC::Ref { listeners }, event_name, list_name, name](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto callback = vm.argument(0);
            if (!callback.is_function())
                return vm.throw_completion<JS::Error>(Utf16String::formatted("Incorrect argument types for webNavigation.{}.{}.", event_name, name));
            auto& entries = listeners->entries().ensure(event_name);
            auto index = entries.find_first_index_if([&](auto const& entry) { return entry.callback.ptr() == &callback.as_function(); });
            if (name == "hasListener"sv)
                return JS::Value { index.has_value() };
            if (index.has_value()) {
                context->event_listeners(list_name).remove(entries[*index].wrapper);
                entries.remove(*index);
            }
            return JS::js_undefined(); }, 1, attributes);
    }
    return object;
}

GC::Ref<JS::Object> create_web_navigation(JS::Realm& realm, MessagingContext& context)
{
    auto web_navigation = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    auto listeners = realm.heap().allocate<WebNavigationListeners>();

    // onTabReplaced is never fired: nothing here replaces one tab's contents with another's.
    for (auto event : { "onBeforeNavigate"sv, "onCommitted"sv, "onDOMContentLoaded"sv, "onCompleted"sv, "onErrorOccurred"sv, "onCreatedNavigationTarget"sv, "onReferenceFragmentUpdated"sv, "onHistoryStateUpdated"sv, "onTabReplaced"sv })
        web_navigation->define_direct_property(Utf16FlyString::from_utf8(event), create_web_navigation_event(realm, context, listeners, event), attributes);

    for (auto method : { "getFrame"sv, "getAllFrames"sv }) {
        web_navigation->define_native_function(realm, Utf16FlyString::from_utf8(method), [context = GC::Ref { context }, method](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            auto details = vm.argument(0);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(1).is_function())
                callback = vm.argument(1).as_function();
            if (!details.is_object() || details.is_function())
                return vm.throw_completion<JS::Error>(Utf16String::formatted("Incorrect argument types for webNavigation.{}.", method));
            JsonObject object;
            for (auto key : { "tabId"sv, "frameId"sv }) {
                auto value = TRY(details.as_object().get(Utf16FlyString::from_utf8(key)));
                if (value.is_integral_number())
                    object.set(key, static_cast<i64>(value.as_double()));
            }
            if (!object.has("tabId"sv))
                return vm.throw_completion<JS::Error>(Utf16String::formatted("Type error for parameter details (Property \"tabId\" is required) for webNavigation.{}.", method));
            JsonArray arguments;
            arguments.must_append(move(object));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("webNavigation.{}", method)), arguments)); }, 1, attributes);
    }
    return web_navigation;
}

}
