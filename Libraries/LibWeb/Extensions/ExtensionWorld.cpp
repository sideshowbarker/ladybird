/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/Extensions/ExtensionWorldEnvironmentSettingsObject.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/Namespace.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionWorld);

GC::Ref<ExtensionWorld> ExtensionWorld::create(DOM::Document& document, ExtensionDescriptor const& descriptor)
{
    auto& vm = Bindings::main_thread_vm();
    auto window = document.window();
    VERIFY(window);
    auto browsing_context = document.browsing_context();
    VERIFY(browsing_context);

    auto world = vm.heap().allocate<ExtensionWorld>(document, descriptor);
    world->m_wrapper_world = vm.heap().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Extension);

    // The global is this world's wrapper of the document's own Window, and the global "this" is this world's proxy
    // for the browsing context, so the script sees the same window the page does through wrappers of its own.
    auto execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        document.relevant_settings_object().agent_cluster_cross_origin_isolation_mode(),
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return browsing_context->window_proxy_for(*world->m_wrapper_world, realm)->object();
        });
    auto& realm = *execution_context->realm();
    world->m_realm = realm;
    world->m_settings = ExtensionWorldEnvironmentSettingsObject::create(document, move(execution_context));

    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    realm.set_host_defined(realm.heap().allocate<Bindings::HostDefined>(intrinsics, *world->m_wrapper_world, window->principal_realm()));
    Bindings::host_defined_of(realm).own_settings_object = world->m_settings;
    Bindings::cache_global_object_wrapper(realm);
    MUST(Bindings::initialize_window_web_interfaces(*window, realm));

    world->m_message_listeners = vm.heap().allocate<EventListenerList>();
    install_namespace(realm, *world, NamespaceContext::ContentScript);

    // self.wrappedJSObject is the page's own window, past this world's wrappers: a content script reads the globals
    // the page's scripts set through it, as a Firefox content script does. The page's realm has no such property.
    realm.global_object().define_native_accessor(
        realm, "wrappedJSObject"_utf16_fly_string, GC::create_function(realm.heap(), GC::captures(GC::Ref<JS::Object> { window->principal_realm().global_object() }), [](auto& page_window, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> { return JS::Value { page_window.ptr() }; }), {}, JS::Attribute::Configurable);
    MessageRouter::the().register_context(*world);
    return world;
}

ExtensionWorld::ExtensionWorld(DOM::Document& document, ExtensionDescriptor descriptor)
    : m_document(document)
    , m_descriptor(move(descriptor))
{
}

ExtensionWorld::~ExtensionWorld() = default;

Page& ExtensionWorld::page()
{
    return m_document->page();
}

MessageSender ExtensionWorld::sender() const
{
    return MessageSender { .extension_id = m_descriptor.id, .url = m_document->url().serialize(), .from_background = false };
}

void ExtensionWorld::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_realm);
    visitor.visit(m_wrapper_world);
    visitor.visit(m_message_listeners);
    visitor.visit(m_settings);
}

JS::Completion ExtensionWorld::run_script(ByteString filename, Utf16View source, URL::URL base_url, HTML::ClassicScript::RethrowErrors rethrow_errors)
{
    if (m_detached)
        return JS::throw_completion(JS::js_undefined());

    auto script = HTML::ClassicScript::create_in_realm(move(filename), Utf16String::from_utf16(source), *m_settings, *m_realm, move(base_url));
    return script->run(rethrow_errors);
}

void ExtensionWorld::detach()
{
    if (m_detached)
        return;
    m_detached = true;
    MessageRouter::the().unregister_context(*this);
    m_wrapper_world->detach();
    m_message_listeners->revoke();
    m_settings = nullptr;
    m_realm = nullptr;
}

}
