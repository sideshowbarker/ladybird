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
#include <LibWeb/Extensions/API/Commands.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

GC::Ref<JS::Object> create_commands(JS::Realm& realm, MessagingContext& context)
{
    auto commands = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    // No shortcut is bound yet, so these never fire.
    for (auto name : { "onCommand"sv, "onChanged"sv })
        commands->define_direct_property(Utf16FlyString::from_utf8(name), create_event(realm, context.event_listeners(MUST(String::formatted("commands.{}", name)))), attributes);

    commands->define_native_function(realm, "getAll"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(0).is_function())
            callback = vm.argument(0).as_function();
        return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "commands.getAll"sv, {})); }, 0, attributes);

    commands->define_native_function(realm, "update"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        auto details = vm.argument(0);
        if (!details.is_object() || details.is_function() || is<JS::Array>(details.as_object()))
            return vm.throw_completion<JS::TypeError>(Utf16String::formatted("Error in invocation of commands.update(object detail): Error at parameter 'detail': Invalid type: expected object, found {}.", api_type_name(details)));
        JsonObject detail;
        for (auto key : { "name"sv, "description"sv, "shortcut"sv }) {
            if (auto value = TRY(details.as_object().get(Utf16FlyString::from_utf8(key))); value.is_string())
                detail.set(key, value.as_string().utf16_string().to_utf8());
        }
        JsonArray arguments;
        arguments.must_append(move(detail));
        return settle_api_call(*context, {}, MessageRouter::the().call_api(*context, "commands.update"sv, arguments)); }, 1, attributes);
    return commands;
}

}
