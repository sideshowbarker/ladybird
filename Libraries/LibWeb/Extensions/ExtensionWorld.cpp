/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
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
    world->m_execution_context = Bindings::create_a_new_javascript_realm(
        vm,
        document.relevant_settings_object().agent_cluster_cross_origin_isolation_mode(),
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Bindings::create_global_object_wrapper(realm, GC::Ref { *window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return browsing_context->window_proxy_for(*world->m_wrapper_world, realm)->object();
        });
    auto& realm = *world->m_execution_context->realm;
    world->m_realm = realm;

    auto intrinsics = realm.create<Bindings::Intrinsics>(realm);
    realm.set_host_defined(realm.heap().allocate<Bindings::HostDefined>(intrinsics, *world->m_wrapper_world, window->principal_realm()));
    Bindings::cache_global_object_wrapper(realm);
    MUST(Bindings::initialize_window_web_interfaces(*window, realm));

    world->m_message_listeners = vm.heap().allocate<EventListenerList>();
    install_namespace(realm, *world, NamespaceContext::ContentScript);
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
    if (m_execution_context)
        m_execution_context->visit_edges(visitor);
}

JS::Completion ExtensionWorld::run_script(ByteString filename, Utf16View source, URL::URL base_url, HTML::ClassicScript::RethrowErrors rethrow_errors)
{
    if (m_detached)
        return JS::throw_completion(JS::js_undefined());

    auto& settings = Bindings::principal_host_defined_environment_settings_object(*m_realm);
    auto script = HTML::ClassicScript::create_in_realm(move(filename), source, settings, *m_realm, move(base_url));
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
    m_execution_context = nullptr;
    m_realm = nullptr;
}

}
