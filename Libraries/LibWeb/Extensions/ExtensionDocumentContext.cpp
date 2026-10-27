/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/ExtensionDocumentContext.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/MessageRouter.h>
#include <LibWeb/Extensions/Namespace.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionDocumentContext);

GC::Ptr<ExtensionDocumentContext> ExtensionDocumentContext::create_if_extension_document(DOM::Document& document)
{
    auto const& url = document.url();
    if (url.scheme() != scheme)
        return nullptr;
    auto const* extension = ExtensionSet::the().find(url.serialized_host());
    if (!extension)
        return nullptr;

    auto context = document.heap().allocate<ExtensionDocumentContext>(document, *extension);
    context->m_message_listeners = document.heap().allocate<EventListenerList>();
    install_namespace(HTML::relevant_realm(document), *context, NamespaceContext::ExtensionDocument);
    MessageRouter::the().register_context(*context);
    return context;
}

ExtensionDocumentContext::ExtensionDocumentContext(DOM::Document& document, ExtensionDescriptor descriptor)
    : m_document(document)
    , m_descriptor(move(descriptor))
{
}

void ExtensionDocumentContext::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_message_listeners);
}

JS::Realm& ExtensionDocumentContext::realm()
{
    return HTML::relevant_realm(*m_document);
}

Page& ExtensionDocumentContext::page()
{
    return m_document->page();
}

MessageSender ExtensionDocumentContext::sender() const
{
    auto const& url = m_document->url();
    return MessageSender {
        .extension_id = m_descriptor.id,
        .url = url.serialize(),
        .from_background = url.serialize_path() == generated_background_page_path,
    };
}

void ExtensionDocumentContext::detach()
{
    if (m_detached)
        return;
    m_detached = true;
    MessageRouter::the().unregister_context(*this);
    m_message_listeners->revoke();
    if (auto listeners = web_request_listeners())
        listeners->revoke();
}

}
