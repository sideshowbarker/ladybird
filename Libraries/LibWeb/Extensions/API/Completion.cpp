/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Completion.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>

namespace Web::Extensions {

JS::Value settle_api_call(MessagingContext& context, GC::Ptr<JS::FunctionObject> callback, GC::Ref<WebIDL::Promise> promise)
{
    if (!callback)
        return promise->promise();

    auto invoke = [context = GC::Ref { context }, callback = GC::Ref { *callback }](JS::Value value, bool failed) -> WebIDL::ExceptionOr<JS::Value> {
        if (context->is_detached())
            return JS::js_undefined();
        auto& realm = context->realm();
        if (failed) {
            auto error = JS::Object::create(realm, realm.intrinsics().object_prototype());
            auto message = value;
            if (value.is_object()) {
                auto property = value.as_object().get("message"_utf16_fly_string);
                if (context->is_detached())
                    return JS::js_undefined();
                if (!property.is_error())
                    message = property.value();
            }
            auto text = message.to_utf16_string(realm.vm());
            if (context->is_detached())
                return JS::js_undefined();
            error->define_direct_property("message"_utf16_fly_string,
                JS::PrimitiveString::create(realm.vm(), text.is_error() ? "The API call failed"_utf16 : text.release_value()), JS::Attribute::Enumerable);
            value = error;
        }
        auto previous = GC::make_root(context->last_error());
        context->set_last_error(failed ? value : JS::js_undefined());
        ScopeGuard restore = [&] { context->set_last_error(previous.value()); };
        auto completion = failed ? JS::call(realm.vm(), *callback, JS::js_undefined()) : JS::call(realm.vm(), *callback, JS::js_undefined(), value);
        if (completion.is_error())
            HTML::report_exception(completion, realm);
        return JS::js_undefined();
    };
    WebIDL::react_to_promise(*promise,
        GC::create_function(context.heap(), [invoke](JS::Value value) mutable { return invoke(value, false); }),
        GC::create_function(context.heap(), [invoke](JS::Value value) mutable { return invoke(value, true); }));
    return JS::js_undefined();
}

}
