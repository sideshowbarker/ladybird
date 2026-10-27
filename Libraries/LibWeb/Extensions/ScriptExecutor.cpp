/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Promise.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/Extensions/ScriptExecutor.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

// An injection calls back once, whichever comes first: its script's result, or its document going away.
class InjectionReply : public RefCounted<InjectionReply> {
public:
    explicit InjectionReply(InjectionCallback callback)
        : m_callback(move(callback))
    {
    }
    bool is_settled() const { return !m_callback; }
    void settle(Optional<String> result_json, Optional<String> error, FrameRemoved frame_removed = FrameRemoved::No)
    {
        if (!m_callback)
            return;
        auto callback = move(m_callback);
        m_callback = nullptr;
        callback(move(result_json), move(error), frame_removed);
    }

private:
    InjectionCallback m_callback;
};

struct QueuedInjection {
    String extension_id;
    ExtensionInjection injection;
    NonnullRefPtr<InjectionReply> reply;
};

// What each document still owes: injections waiting for a run_at point, and those waiting for a promise.
struct DocumentInjections {
    Vector<QueuedInjection> queued;
    Vector<NonnullRefPtr<InjectionReply>> outstanding;
};

static HashMap<HTML::CrossProcessId, DocumentInjections>& document_injections()
{
    static auto& injections = *new HashMap<HTML::CrossProcessId, DocumentInjections>;
    return injections;
}

static Optional<String> json_of(JS::Realm& realm, JS::Value value)
{
    if (value.is_undefined())
        return {};
    auto json = JS::JSONObject::stringify_impl(realm.vm(), value, JS::js_undefined(), JS::js_undefined());
    if (json.is_error() || !json.value().has_value())
        return "null"_string;
    return json.value()->to_utf8();
}

// An error reads as its toString() does ("Error: boom"), which the injected script's own world runs.
static String error_of(JS::VM& vm, JS::Value value)
{
    auto text = value.to_utf16_string(vm);
    if (text.is_error())
        return value.to_utf16_string_without_side_effects().to_utf8_but_should_be_ported_to_utf16();
    return text.value().to_utf8_but_should_be_ported_to_utf16();
}

static void run_injection(DOM::Document& document, ExtensionDescriptor const& extension, ExtensionInjection const& injection, NonnullRefPtr<InjectionReply> reply)
{
    if (!document.window()) {
        reply->settle({}, {}, FrameRemoved::Yes);
        return;
    }
    for (size_t i = 0; i < injection.css_sources.size(); ++i) {
        DOM::ExtensionStyleSheetOwner owner { extension.id, injection.css_keys[i], injection.css_user_origin };
        if (injection.remove_css)
            document.remove_extension_style_sheet(owner);
        else
            document.add_extension_style_sheet(Utf16String::from_utf8(injection.css_sources[i]), document.url(), move(owner));
    }
    if (injection.js_sources.is_empty() && !injection.function_source.has_value()) {
        reply->settle({}, {});
        return;
    }

    // A function runs as an expression called with its arguments, which are a JSON array.
    Vector<String> names = injection.js_paths;
    Vector<String> sources = injection.js_sources;
    if (injection.function_source.has_value()) {
        names = { "function"_string };
        sources = { MUST(String::formatted("({})(...{})", *injection.function_source, injection.arguments_json.is_empty() ? "[]"sv : injection.arguments_json.bytes_as_string_view())) };
    }

    GC::Ptr<JS::Realm> realm;
    if (injection.world == ExecutionWorld::Main)
        realm = HTML::relevant_realm(*document.window());
    else
        realm = document.ensure_extension_world(extension).realm();
    HTML::TemporaryExecutionContext execution_context { *realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // The value of the last source is the result, as each runs in turn.
    JS::Value result = JS::js_undefined();
    for (size_t i = 0; i < sources.size(); ++i) {
        auto source = Utf16String::from_utf8(sources[i]);
        JS::Completion completion = [&] {
            if (injection.world == ExecutionWorld::Main) {
                auto& settings = HTML::relevant_settings_object(*document.window());
                auto script = HTML::ClassicScript::create(names[i].to_byte_string(), source, settings, document.url());
                return script->run(HTML::ClassicScript::RethrowErrors::Yes);
            }
            return document.ensure_extension_world(extension).run_script(names[i].to_byte_string(), source, document.url(), HTML::ClassicScript::RethrowErrors::Yes);
        }();
        if (completion.is_abrupt()) {
            reply->settle({}, error_of(realm->vm(), completion.value()));
            return;
        }
        result = completion.value();
    }

    if (!result.is_object() || !is<JS::Promise>(result.as_object())) {
        reply->settle(json_of(*realm, result), {});
        return;
    }
    // A promise that hasn't settled by the time the document goes away answers with the frame's removal instead.
    auto& outstanding = document_injections().ensure(document.id()).outstanding;
    outstanding.remove_all_matching([](auto const& reply) { return reply->is_settled(); });
    outstanding.append(reply);
    auto promise = WebIDL::create_resolved_promise(*realm, result);
    WebIDL::react_to_promise(promise,
        GC::create_function(realm->heap(), [realm, reply](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
            reply->settle(json_of(*realm, value), {});
            return JS::js_undefined();
        }),
        GC::create_function(realm->heap(), [realm, reply](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
            reply->settle({}, error_of(realm->vm(), reason));
            return JS::js_undefined();
        }));
}

void execute_injection(DOM::Document& document, ExtensionDescriptor const& extension, ExtensionInjection const& injection, InjectionCallback callback)
{
    auto reply = make_ref_counted<InjectionReply>(move(callback));
    if (injection.run_at.has_value() && !document.content_scripts_ran_at(*injection.run_at)) {
        document_injections().ensure(document.id()).queued.append({ extension.id, injection, move(reply) });
        return;
    }
    run_injection(document, extension, injection, move(reply));
}

void run_pending_injections(DOM::Document& document, RunAt reached)
{
    if (!document.has_id())
        return;
    auto injections = document_injections().find(document.id());
    if (injections == document_injections().end())
        return;
    Vector<QueuedInjection> due;
    injections->value.queued.remove_all_matching([&](QueuedInjection& queued) {
        if (to_underlying(*queued.injection.run_at) > to_underlying(reached))
            return false;
        due.append(move(queued));
        return true;
    });
    for (auto& queued : due) {
        // The extension may have been disabled or reloaded while its injection waited.
        auto const* extension = ExtensionSet::the().find(queued.extension_id);
        if (!extension) {
            queued.reply->settle({}, {}, FrameRemoved::Yes);
            continue;
        }
        run_injection(document, *extension, queued.injection, queued.reply);
        if (!document.is_fully_active())
            break;
    }
}

void cancel_pending_injections(DOM::Document& document)
{
    if (!document.has_id())
        return;
    auto injections = document_injections().take(document.id());
    if (!injections.has_value())
        return;
    for (auto& queued : injections->queued)
        queued.reply->settle({}, {}, FrameRemoved::Yes);
    for (auto& reply : injections->outstanding)
        reply->settle({}, {}, FrameRemoved::Yes);
}

}
