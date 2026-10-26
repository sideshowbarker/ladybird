/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/I18n.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWebCommon/Extensions/Localization.h>

namespace Web::Extensions {

// The substitutions argument is a single string or a real array of at most nine values, and anything else stands for
// none. An element whose getter throws gives the call no result at all, but one that can't become a string just drops
// out of the list.
static JS::ThrowCompletionOr<Optional<Vector<String>>> read_substitutions(JS::VM& vm, JS::Value argument)
{
    if (argument.is_string())
        return Vector { argument.as_string().utf16_string().to_utf8() };
    if (!argument.is_object() || !is<JS::Array>(argument.as_object()))
        return Vector<String> {};
    auto& array = argument.as_object();
    auto count = TRY(JS::length_of_array_like(vm, array));
    if (count > 9)
        return OptionalNone {};
    Vector<String> strings;
    for (size_t index = 0; index < count; ++index) {
        auto element = array.get(index);
        if (element.is_error())
            return OptionalNone {};
        if (auto string = element.value().to_utf16_string(vm); !string.is_error())
            strings.append(string.value().to_utf8());
    }
    return strings;
}

// Only an "escapeLt" that's exactly true turns escaping on; a getter that throws leaves it off.
static bool escapes_less_than_signs(JS::Value options)
{
    if (!options.is_object())
        return false;
    auto escape_lt = options.as_object().get("escapeLt"_utf16_fly_string);
    return !escape_lt.is_error() && escape_lt.value().is_boolean() && escape_lt.value().as_bool();
}

GC::Ref<JS::Object> create_i18n(JS::Realm& realm, MessagingContext& context)
{
    auto i18n = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    i18n->define_native_function(
        realm, "getMessage"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            if (!vm.argument(0).is_string())
                return vm.throw_completion<JS::TypeError>("The message name must be a string"sv);
            auto options = vm.argument(2);
            if (!options.is_nullish() && !options.is_object())
                return vm.throw_completion<JS::TypeError>("The getMessage() options must be an object"sv);
            auto name = vm.argument(0).as_string().utf16_string().to_utf8().to_ascii_lowercase();
            auto substitutions = TRY(read_substitutions(vm, vm.argument(1)));
            if (!substitutions.has_value())
                return JS::js_undefined();
            auto message = context->descriptor().messages.get(name).value_or(String {});
            auto formatted = format_message(message, *substitutions, escapes_less_than_signs(options));
            return JS::PrimitiveString::create(vm, Utf16String::from_utf8(formatted.release_value()));
        },
        1, attributes);
    i18n->define_native_function(
        realm, "getUILanguage"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
            return JS::PrimitiveString::create(vm, Utf16String::from_utf8(context->descriptor().ui_locale));
        },
        0, attributes);
    return i18n;
}

}
