/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// One of an extension's own documents, such as its background page: its principal realm carries the "browser"
// namespace, and it sends and receives runtime messages like a content script's world does.
class WEB_API ExtensionDocumentContext final : public MessagingContext {
    GC_CELL(ExtensionDocumentContext, MessagingContext);
    GC_DECLARE_ALLOCATOR(ExtensionDocumentContext);

public:
    // Null unless the document is one of a loaded extension's, at a ladybird-extension://<id>/ URL.
    static GC::Ptr<ExtensionDocumentContext> create_if_extension_document(DOM::Document&);

    virtual DOM::Document& document() override { return *m_document; }
    virtual JS::Realm& realm() override;
    virtual ExtensionDescriptor const& descriptor() const override { return m_descriptor; }
    virtual EventListenerList& message_listeners() override { return *m_message_listeners; }
    virtual Page& page() override;
    virtual MessageSender sender() const override;
    virtual bool is_detached() const override { return m_detached; }

    virtual void detach() override;

private:
    ExtensionDocumentContext(DOM::Document&, ExtensionDescriptor);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    ExtensionDescriptor m_descriptor;
    GC::Ptr<EventListenerList> m_message_listeners;
    bool m_detached { false };
};

}
