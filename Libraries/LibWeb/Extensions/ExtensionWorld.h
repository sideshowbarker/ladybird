/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Scripting/ClassicScript.h>

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// One extension's world in one document: a realm whose global is another wrapper of the document's Window, with its
// own wrapper cache, so a content script sees the live DOM through its own wrappers and nothing the page put on its
// objects. The realm carries the "browser" namespace, and the scripts run with the document's settings object.
class WEB_API ExtensionWorld final : public MessagingContext {
    GC_CELL(ExtensionWorld, MessagingContext);
    GC_DECLARE_ALLOCATOR(ExtensionWorld);

public:
    static GC::Ref<ExtensionWorld> create(DOM::Document&, ExtensionDescriptor const&);
    virtual ~ExtensionWorld() override;

    virtual JS::Realm& realm() override { return *m_realm; }
    virtual ExtensionDescriptor const& descriptor() const override { return m_descriptor; }
    virtual void set_descriptor(ExtensionDescriptor descriptor) override { m_descriptor = move(descriptor); }
    virtual EventListenerList& message_listeners() override { return *m_message_listeners; }
    virtual Page& page() override;
    virtual MessageSender sender() const override;
    virtual bool is_detached() const override { return m_detached; }

    virtual DOM::Document& document() override { return *m_document; }

    // Runs the source as a classic script of this world. A detached world refuses with an abrupt completion.
    // An injected script asks for its exception back; a content script's is reported.
    JS::Completion run_script(ByteString filename, Utf16View source, URL::URL base_url, HTML::ClassicScript::RethrowErrors = HTML::ClassicScript::RethrowErrors::No);

    // Drops the realm and its wrapper cache. Every world is detached before its document is destroyed.
    virtual void detach() override;

private:
    ExtensionWorld(DOM::Document&, ExtensionDescriptor);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    ExtensionDescriptor m_descriptor;
    OwnPtr<JS::ExecutionContext> m_execution_context;
    GC::Ptr<JS::Realm> m_realm;
    GC::Ptr<Bindings::WrapperWorld> m_wrapper_world;
    GC::Ptr<EventListenerList> m_message_listeners;
    bool m_detached { false };
};

}
