/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/MessageSender.h>

namespace Web::Extensions {

// Routes runtime messages within one process: a send goes to the page client (and so to the UI process, which fans
// it out to the extension's other contexts), a reply completes the sender's promise, and a dispatch from the UI
// process runs the addressed context's onMessage listeners and reports the first reply, or that nobody replied.
class ReplyCollector;

class WEB_API MessageRouter {
public:
    static MessageRouter& the();

    void register_context(MessagingContext&);
    void unregister_context(MessagingContext&);

    // runtime.sendMessage(message): serializes the message in the context's realm and returns the promise of a reply.
    GC::Ref<JS::PromiseCapability> send(MessagingContext&, JS::Value message);

    // The reply to a message this process sent: the JSON of the reply, or nothing when no listener replied.
    void complete(Page&, u64 request_id, Optional<String> const& reply_json);

    // A message addressed to one registered context of the extension on this page.
    void dispatch(Page&, String const& extension_id, u64 dispatch_id, MessageSender const&, String const& message_json, u64 context_id);

    void finished_dispatch(u64 dispatch_id);

private:
    MessageRouter();
    void reconcile_contexts();

    struct PendingSend {
        GC::Root<JS::PromiseCapability> promise;
        GC::Root<JS::Realm> realm;
        Page* page { nullptr };
        MessagingContext* context { nullptr };
    };

    struct PendingDispatch {
        GC::Root<ReplyCollector> collector;
        MessagingContext* context;
    };

    HashTable<MessagingContext*> m_contexts;
    HashMap<u64, PendingDispatch> m_pending_dispatches;
    HashMap<u64, PendingSend> m_pending_sends;
    u64 m_next_request_id { 1 };
};

}
