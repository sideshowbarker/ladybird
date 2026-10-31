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
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/API/Privacy.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// The settings an extension's BrowserSetting objects stand for, by their names under browser.privacy.
struct PrivacyGroup {
    StringView name;
    ReadonlySpan<StringView> settings;
};
static constexpr Array network_settings { "networkPredictionEnabled"sv, "peerConnectionEnabled"sv, "webRTCIPHandlingPolicy"sv };
static constexpr Array websites_settings { "hyperlinkAuditingEnabled"sv };

// Extensions compare these messages, so their wording is fixed.
static JS::ThrowCompletionOr<void> throw_details_error(JS::VM& vm, StringView method, StringView detail)
{
    return vm.throw_completion<JS::Error>(Utf16String::formatted("Type error for parameter details ({}) for {}.", detail, method));
}

// get(), set(), and clear() take a details object, and set()'s has to have a value; a scope, if there's one, has to be
// one the API names, and Hub refuses any but "regular".
static JS::ThrowCompletionOr<JsonObject> parse_details(JS::VM& vm, StringView method, JS::Value value, bool needs_value)
{
    if (!value.is_object() || value.is_function() || is<JS::Array>(value.as_object()))
        TRY(throw_details_error(vm, method, "Expected object instead of a value of another type"sv));
    auto& object = value.as_object();
    JsonObject details;
    if (needs_value) {
        auto setting_value = TRY(object.get("value"_utf16_fly_string));
        if (setting_value.is_undefined())
            TRY(throw_details_error(vm, method, "Property \"value\" is required"sv));
        auto json = TRY(JS::JSONObject::stringify_impl(vm, setting_value, JS::js_undefined(), JS::js_undefined()));
        if (!json.has_value())
            TRY(throw_details_error(vm, method, "Error processing value: Expected a JSON value"sv));
        details.set("value"sv, MUST(JsonValue::from_string(json->to_utf8())));
    }
    if (auto scope = TRY(object.get("scope"_utf16_fly_string)); !scope.is_undefined()) {
        static constexpr Array scopes { "regular"sv, "regular_only"sv, "incognito_persistent"sv, "incognito_session_only"sv };
        auto name = scope.is_string() ? scope.as_string().utf16_string().to_utf8() : String {};
        if (!scope.is_string() || !scopes.span().contains_slow(name.bytes_as_string_view()))
            TRY(throw_details_error(vm, method, MUST(String::formatted("Error processing scope: Invalid enumeration value {}", JsonValue { name }.serialized()))));
        details.set("scope"sv, name);
    }
    return details;
}

static GC::Ref<JS::Object> create_browser_setting(JS::Realm& realm, MessagingContext& context, String const& name)
{
    auto setting = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    struct Method {
        GC_HOLDS_NO_CELL_EDGES;
        StringView name;
        bool needs_value;
    };
    for (auto method : { Method { "get"sv, false }, Method { "set"sv, true }, Method { "clear"sv, false } }) {
        setting->define_native_function(realm, Utf16FlyString::from_utf8(method.name), GC::create_function(realm.heap(), GC::captures(GC::Ref { context }, name, method), [](auto& context, auto& name, auto& method, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(1).is_function())
                callback = vm.argument(1).as_function();
            auto qualified = MUST(String::formatted("privacy.{}.{}", name, method.name));
            JsonArray arguments;
            arguments.must_append(name);
            arguments.must_append(TRY(parse_details(vm, qualified, vm.argument(0), method.needs_value)));
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, MUST(String::formatted("privacy.{}", method.name)), arguments)); }), 1, attributes);
    }
    setting->define_direct_property("onChange"_utf16_fly_string, create_event(realm, context.event_listeners(MUST(String::formatted("privacy.{}.onChange", name)))), attributes);
    return setting;
}

GC::Ref<JS::Object> create_privacy(JS::Realm& realm, MessagingContext& context)
{
    auto privacy = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    for (auto group : { PrivacyGroup { "network"sv, network_settings }, PrivacyGroup { "websites"sv, websites_settings } }) {
        auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());
        for (auto setting : group.settings)
            object->define_direct_property(Utf16FlyString::from_utf8(setting), create_browser_setting(realm, context, MUST(String::formatted("{}.{}", group.name, setting))), attributes);
        privacy->define_direct_property(Utf16FlyString::from_utf8(group.name), object, attributes);
    }
    return privacy;
}

}
