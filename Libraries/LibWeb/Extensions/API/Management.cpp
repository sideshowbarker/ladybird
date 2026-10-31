/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/Management.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/MessagingContext.h>

namespace Web::Extensions {

// Only getSelf(): an extension may read its own ExtensionInfo, and nothing about any other.
GC::Ref<JS::Object> create_management(JS::Realm& realm, MessagingContext& context)
{
    auto management = JS::Object::create(realm, realm.intrinsics().object_prototype());
    management->define_native_function(
        realm, "getSelf"_utf16_fly_string, GC::create_function(realm.heap(), GC::captures(GC::Ref { context }), [](auto& context, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            if (context->is_detached())
                return vm.throw_completion<JS::TypeError>(u"The extension context is gone"sv);
            GC::Ptr<JS::FunctionObject> callback;
            if (vm.argument(0).is_function())
                callback = vm.argument(0).as_function();
            return settle_api_call(*context, callback, MessageRouter::the().call_api(*context, "management.getSelf"sv, {}));
        }),
        0, JS::Attribute::Writable | JS::Attribute::Configurable);
    return management;
}

}
