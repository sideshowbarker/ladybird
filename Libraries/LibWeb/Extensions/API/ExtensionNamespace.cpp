/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/API/ExtensionNamespace.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

GC::Ref<JS::Object> create_extension_namespace(JS::Realm& realm, MessagingContext& context)
{
    auto extension = JS::Object::create(realm, realm.intrinsics().object_prototype());
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;
    // Ladybird doesn't keep file access apart from host permissions, so an extension may reach file: URLs whenever its
    // host permissions let it.
    extension->define_native_function(realm, "isAllowedFileSchemeAccess"_utf16_fly_string, [context = GC::Ref { context }](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        if (context->is_detached())
            return vm.throw_completion<JS::TypeError>("The extension context is gone"sv);
        GC::Ptr<JS::FunctionObject> callback;
        if (vm.argument(0).is_function())
            callback = vm.argument(0).as_function();
        return settle_api_call(*context, callback, WebIDL::create_resolved_promise(context->realm(), JS::Value(true))); }, 0, attributes);
    return extension;
}

}
