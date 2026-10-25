/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibWebCommon/Extensions/MessageResult.h>
#include <LibWebCommon/Extensions/MessageSender.h>
#include <LibWebCommon/Extensions/Storage.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Routes runtime messages between an extension's contexts across WebContent processes. A page registers each
// context with its document identity; a message goes to each receiving context under its own dispatch id. The first
// answer settles the sender's request, and once every target declines or goes away, the request has no reply.
class WEBVIEW_API ExtensionMessageBroker : public Weakable<ExtensionMessageBroker> {
    AK_ALLOC_WITH_KMALLOC;

public:
    // A page the broker can deliver to: a WebContent page in production, a stand-in in tests. An endpoint that goes
    // away tells the broker so through endpoint_gone().
    class Endpoint {
    public:
        virtual ~Endpoint() = default;
        virtual bool extension_has_permission(String const&, StringView) const { return false; }
        virtual void complete_extension_storage(u64, Web::Extensions::StorageResult const&) { }
        virtual void dispatch_extension_storage_changed(String const&, String const&) { }
        virtual Optional<u64> extension_revision(String const& extension_id) const = 0;
        virtual Optional<Web::Extensions::MessageSender> extension_message_sender(String const& extension_id, Web::HTML::CrossProcessId document_id) const = 0;
        virtual void dispatch_extension_runtime_message(String const& extension_id, u64 dispatch_id, Web::Extensions::MessageSender const&, String const& message_json, u64 context_id) = 0;
        virtual void complete_extension_runtime_message(u64 request_id, Web::Extensions::MessageResult const& reply_json) = 0;
    };

    void context_created(Endpoint&, String const& extension_id, u64 context_id, Web::HTML::CrossProcessId document_id, u64 revision);
    void context_destroyed(Endpoint&, String const& extension_id, u64 context_id);
    void endpoint_gone(Endpoint&);
    void reconcile_contexts();
    void background_starting(String const& extension_id, u64 revision);
    void background_finished(String const& extension_id, u64 revision, bool success);

    void send(Endpoint& source, String const& extension_id, u64 request_id, Web::Extensions::MessageSender const&, String const& message_json, u64 context_id);
    void reply(Endpoint& target, u64 dispatch_id, Web::Extensions::MessageResult const& reply_json);

    void request_storage(Endpoint&, u64 context_id, u64 request_id, Web::Extensions::StorageRequest const&, ExtensionStorageStore&);

private:
    struct Context;
    struct PendingRequest {
        Endpoint* source { nullptr };
        u64 request_id { 0 };
        u64 source_context_id { 0 };
        size_t outstanding { 0 };
        bool had_listener { false };
    };

    bool storage_context_is_current(Endpoint*, u64 context_id, Context const&) const;
    void broadcast_storage_changes(String const&, u64 revision, String const&);
    void settle(u64 pending_id, Web::Extensions::MessageResult const& reply_json);
    void decline(u64 pending_id);

    void drop_request(u64 pending_id);

    struct Context {
        String extension_id;
        Web::HTML::CrossProcessId document_id;
        u64 revision;
    };
    struct Target {
        Endpoint* endpoint;
        u64 context_id;
    };
    HashMap<Endpoint*, HashMap<u64, Context>> m_contexts;
    HashMap<u64, PendingRequest> m_pending_requests;
    // Each dispatch id maps to the pending request it belongs to and the endpoint it went to.
    HashMap<u64, u64> m_dispatch_to_pending;
    HashMap<u64, Target> m_dispatch_to_endpoint;
    struct QueuedMessage {
        Endpoint* source;
        String extension_id;
        u64 request_id;
        u64 context_id;
        u64 revision;
        String json;
    };
    Vector<QueuedMessage> m_queued_messages;
    HashMap<String, u64> m_starting_backgrounds;
    u64 m_next_pending_id { 1 };
    u64 m_next_dispatch_id { 1 };
};

}
