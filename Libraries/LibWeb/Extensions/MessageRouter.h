/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/MessageResult.h>
#include <LibWebCommon/Extensions/MessageSender.h>
#include <LibWebCommon/Extensions/Storage.h>

namespace Web::Extensions {

// Routes runtime messages within one process: a send goes to the page client (and so to the UI process, which fans
// it out to the extension's other contexts), a reply completes the sender's promise, and a dispatch from the UI
// process runs the addressed context's onMessage listeners and reports the first reply, or that nobody replied.
class ReplyCollector;

class WEB_API MessageRouter {
public:
    AK_ALLOC_WITH_KMALLOC;

    static MessageRouter& the();

    void register_context(MessagingContext&);
    void unregister_context(MessagingContext&);

    // runtime.sendMessage(message): serializes the message in the context's realm and returns the promise of a reply.
    GC::Ref<JS::PromiseCapability> send(MessagingContext&, JS::Value message);

    // The result of a message this process sent: a JSON reply, a decline, or an error.
    void complete(Page&, u64 request_id, Web::Extensions::MessageResult const& reply_json);

    // A message addressed to one registered context of the extension on this page.
    void dispatch(Page&, String const& extension_id, u64 dispatch_id, MessageSender const&, String const& message_json, u64 context_id);

    GC::Ref<JS::PromiseCapability> request_storage(MessagingContext&, StorageRequest const&, Optional<JsonObject> defaults = {});

    // tabs.query(): the UI process answers with the matching tabs, through complete().
    GC::Ref<JS::PromiseCapability> query_tabs(MessagingContext&, String const& query_json);
    // tabs.sendMessage(): like send(), but to the extension's content scripts in one tab.
    GC::Ref<JS::PromiseCapability> send_to_tab(MessagingContext&, i64 tab_id, Optional<i64> frame_id, JS::Value message);
    void complete_storage(Page&, u64, StorageResult const&);
    void dispatch_storage_changed(Page&, String const&, String const&);

    GC::Ref<JS::Object> connect(MessagingContext&, Optional<String> target_extension_id, String const& name);
    void dispatch_connect(Page&, u64 context_id, u64 channel_id, u64 port_id, String const& name, MessageSender const&);
    void complete_connect(Page&, u64 context_id, u64 port_id, u64 channel_id, Optional<String> const& error);
    void dispatch_port_message(Page&, u64 context_id, u64 channel_id, u64 port_id, String const& json);
    void dispatch_port_disconnect(Page&, u64 context_id, u64 port_id, Optional<String> const& error);

    void finished_dispatch(u64 dispatch_id);

private:
    MessageRouter();
    MessagingContext* context_for(Page&, u64 context_id);
    void reconcile_contexts();

    struct PendingSend {
        GC::Root<JS::PromiseCapability> promise;
        GC::Root<JS::Realm> realm;
        GC::RawPtr<Page> page;
        GC::RawPtr<MessagingContext> context;
    };

    struct PendingDispatch {
        GC::Root<ReplyCollector> collector;
        GC::RawPtr<MessagingContext> context;
    };

    struct PendingStorage {
        PendingSend sender;
        Optional<JsonObject> defaults;
    };
    HashMap<u64, PendingStorage> m_pending_storage;
    HashTable<GC::RawPtr<MessagingContext>> m_contexts;
    HashMap<u64, GC::RawPtr<MessagingContext>> m_contexts_by_id;
    u64 m_next_port_id { 1 };
    HashMap<u64, PendingDispatch> m_pending_dispatches;
    HashMap<u64, PendingSend> m_pending_sends;
    u64 m_next_request_id { 1 };
};

}
