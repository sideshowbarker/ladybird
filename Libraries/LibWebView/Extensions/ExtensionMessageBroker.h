/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
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
        virtual void dispatch_extension_runtime_connect(u64 context_id, u64 channel_id, u64 port_id, String const& name, Web::Extensions::MessageSender const&) = 0;
        virtual void complete_extension_runtime_connect(u64 context_id, u64 port_id, u64 channel_id, Optional<String> const& error) = 0;
        virtual void dispatch_extension_port_message(u64 context_id, u64 channel_id, u64 port_id, String const& json) = 0;
        virtual void dispatch_extension_port_disconnect(u64 context_id, u64 port_id, Optional<String> const& error) = 0;
        virtual bool extension_has_permission(String const&, StringView) const { return false; }
        virtual void complete_extension_storage(u64, Web::Extensions::StorageResult const&) { }
        virtual void dispatch_extension_storage_changed(String const&, String const&) { }
        virtual Optional<u64> extension_revision(String const& extension_id) const = 0;
        virtual Optional<Web::Extensions::MessageSender> extension_message_sender(String const& extension_id, Web::HTML::CrossProcessId document_id) const = 0;
        // The view of the tab this endpoint's page is part of, and a document's frame id within that tab.
        virtual Optional<u64> extension_view_id() const { return {}; }
        virtual Optional<i64> extension_frame_id(Web::HTML::CrossProcessId) const { return {}; }
        // Adds the sending document's tab and frame to the sender a receiver sees.
        virtual void annotate_extension_sender(Web::Extensions::MessageSender&, Web::HTML::CrossProcessId) const { }
        virtual void dispatch_extension_suspend(String const&, u64, bool) { }
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
    // tabs.sendMessage(): reaches the extension's content scripts in the tab that view_id shows, or in just one frame of it.
    void send_to_tab(Endpoint& source, String const& extension_id, u64 request_id, u64 context_id, u64 view_id, Optional<i64> frame_id, String const& message_json);
    void reply(Endpoint& target, u64 dispatch_id, Web::Extensions::MessageResult const& reply_json);

    void request_storage(Endpoint&, u64 context_id, u64 request_id, Web::Extensions::StorageRequest const&, ExtensionStorageStore&);

    // The extension and document of a context that's still current, for a request that only reads what the browser shows.
    struct CurrentContext {
        String extension_id;
        Web::HTML::CrossProcessId document_id;
        // One of the extension's own documents, rather than a content script's world.
        bool is_extension_document { false };
    };
    Optional<CurrentContext> current_context(Endpoint&, u64 context_id) const;

    void connect(Endpoint&, u64 context_id, u64 port_id, Optional<String> target_extension_id, String name);
    void connect_ack(Endpoint&, u64 context_id, u64 channel_id, bool accepted);
    void port_post_message(Endpoint&, u64 context_id, u64 channel_id, u64 port_id, String json);
    void port_disconnect(Endpoint&, u64 context_id, u64 channel_id, u64 port_id);
    void close_channels_for(String const& extension_id);
    bool has_open_channels(String const& extension_id) const;
    // Whether a message to the extension's background page is still waiting for its answer, and, with a maximum age,
    // has waited for less than that.
    bool has_unanswered_dispatches(String const& extension_id, Optional<AK::Duration> max_age = {}) const;
    // Dispatches runtime.onSuspend, or onSuspendCanceled, to every context of the extension's background page, and
    // returns the contexts it reached.
    Vector<u64> dispatch_suspend(String const& extension_id, bool canceled);

    // The background host's hooks: wake_background starts an extension's stopped background page, so that what's sent
    // to it waits for it to load, and on_activity hears of every message, reply, port message, and storage request, and
    // whether the background page itself made it.
    Function<void(String const& extension_id)> wake_background;
    Function<void(String const& extension_id, bool from_background)> on_activity;

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
    void broadcast_storage_changes(String const&, u64 revision, String const&, bool written_by_background);
    void settle(u64 pending_id, Web::Extensions::MessageResult const& reply_json);
    void decline(u64 pending_id);

    void drop_request(u64 pending_id);

    struct Context {
        String extension_id;
        Web::HTML::CrossProcessId document_id;
        u64 revision;
        HashMap<u64, u64> ports;
    };
    struct Target {
        Endpoint* endpoint;
        u64 context_id;
    };
    void dispatch_to_targets(Endpoint& source, String const& extension_id, u64 request_id, u64 context_id, Web::Extensions::MessageSender const&, String const& message_json, Vector<Target> const&);
    struct PortEndpoint {
        Endpoint* endpoint;
        u64 context_id;
        u64 port_id;
        bool background { false };
        bool dispatched { false };
        bool acknowledged { false };
    };
    struct Channel {
        String extension_id;
        u64 revision;
        String name;
        Web::Extensions::MessageSender sender;
        PortEndpoint opener;
        HashMap<u64, PortEndpoint> receivers;
        Vector<String> queued_messages;
        size_t outstanding { 0 };
        bool waiting_for_background { false };
        bool opening { true };
        bool had_receiver { false };
    };
    bool owns_port(Endpoint&, u64 context_id, u64 channel_id, u64 port_id) const;
    void dispatch_connect(u64 channel_id);
    void finish_opening(u64 channel_id);
    void remove_port(u64 channel_id, PortEndpoint const&);
    void forget_port(u64 channel_id, PortEndpoint const&);
    void close_channel(u64 channel_id, Optional<String> error = {}, bool notify_opener = true);
    HashMap<u64, Channel> m_channels;
    u64 m_next_channel_id { 1 };
    u64 m_next_receiver_port_id { 1ull << 63 };
    HashMap<Endpoint*, HashMap<u64, Context>> m_contexts;
    HashMap<u64, PendingRequest> m_pending_requests;
    // Each dispatch id maps to the pending request it belongs to and the endpoint it went to.
    HashMap<u64, u64> m_dispatch_to_pending;
    HashMap<u64, Target> m_dispatch_to_endpoint;
    HashMap<u64, MonotonicTime> m_dispatch_started;
    void report_activity(String const& extension_id, bool from_background);
    bool is_background_endpoint(Endpoint&, String const& extension_id, Web::HTML::CrossProcessId document_id) const;
    void deliver_storage_changes(Endpoint&, u64 context_id, String const& extension_id, u64 revision, String const& changes);

    // Storage changes for a background page that's starting, delivered once it has loaded.
    struct QueuedStorageChanges {
        String extension_id;
        u64 revision;
        String changes;
    };
    Vector<QueuedStorageChanges> m_queued_storage_changes;

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
