/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibRequests/WebRequest.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/Extensions/WebRequest.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Where a request belongs, as the UI process knows it: its tab and window (-1 outside any tab), its frame (0 for a
// tab's top-level document), and that frame's parent (-1 at the top).
struct WebRequestPlace {
    i64 tab_id { -1 };
    i64 window_id { -1 };
    i64 frame_id { 0 };
    i64 parent_frame_id { -1 };
    // The document a frame's navigation loads in, which the request's own process may not have.
    Optional<URL::URL> parent_document_url;
    bool is_private { false };
    // False when the page or frame the request claims isn't one the UI process knows its process to have.
    bool is_verified { true };
};

// The webRequest listeners of every extension context, by the page hosting the context, and the requests extensions
// have heard about. RequestServer is told which requests any listener wants, and asks about no others.
class WEBVIEW_API ExtensionWebRequestRouter {
    AK_ALLOC_WITH_KMALLOC;

public:
    // A page that hosts extension contexts.
    class Endpoint {
    public:
        virtual ~Endpoint() = default;
        virtual void dispatch_extension_web_request_event(u64 context_id, u64 dispatch_id, Vector<u64> listener_ids, String details_json, bool wants_answer) = 0;
    };

    // The descriptor of an enabled extension, and whether it may see private browsing.
    Function<Web::Extensions::ExtensionDescriptor const*(StringView extension_id)> descriptor_for;
    Function<bool(StringView extension_id)> allowed_in_private;
    // Where the extension comes in the order extensions were installed: a later one has a greater rank.
    Function<size_t(StringView extension_id)> install_rank;
    // The decision about a request that waits on its listeners' answers.
    Function<void(int client_id, u64 request_id, Requests::WebRequestDecision)> on_decision;

    // Sends an event of a RequestServer client's request to every listener that hears it: one whose filter matches,
    // of an extension with host access to the request's URL and to the document that made it. When the request waits,
    // on_decision is called once every blocking listener has answered or its context has gone.
    void report(int client_id, u64 request_id, Requests::WebRequestEventDetails const&, WebRequestPlace const&, bool wants_answer);
    // The answers of a context's blocking listeners to a dispatch, as JSON; a listener that gave none is left out.
    void answers_received(Endpoint&, u64 dispatch_id, Vector<String> answers_json);

    struct ExtensionAnswer {
        String extension_id;
        JsonValue answer;
    };
    // Any cancel wins; otherwise the most recently installed extension's redirect, then its headers, count.
    // A redirect to an extension's own resource counts only if the page making the request may load it.
    Requests::WebRequestDecision merge_answers(Requests::WebRequestEvent, Vector<ExtensionAnswer> const&, Optional<URL::URL> const& initiator = {}) const;

    void listeners_changed(Endpoint&, String const& extension_id, u64 context_id, Vector<Web::Extensions::WebRequestListener>);
    void context_destroyed(Endpoint&, u64 context_id);
    void endpoint_destroyed(Endpoint&);

    // Every distinct filter some listener has, for RequestServer.
    Vector<Requests::WebRequestInterest> interest() const;
    Function<void()> on_interest_changed;

    // The requestId extensions see for a RequestServer client's request: the same for every hop of a request that
    // redirects, and never reused.
    static String request_id_for(int client_id, u64 serial);

private:
    struct ContextKey {
        Endpoint* endpoint { nullptr };
        u64 context_id { 0 };

        bool operator==(ContextKey const&) const = default;
    };
    struct ContextKeyTraits : public DefaultTraits<ContextKey> {
        static unsigned hash(ContextKey const& key) { return pair_int_hash(ptr_hash(key.endpoint), u64_hash(key.context_id)); }
    };
    struct ContextListeners {
        String extension_id;
        Vector<Web::Extensions::WebRequestListener> listeners;
    };
    void notify_interest_changed();
    void settle_dispatch(u64 dispatch_id, Vector<String> answers_json);
    Optional<URL::URL> valid_redirect_target(String const& extension_id, StringView url, Optional<URL::URL> const& initiator) const;
    void settle_dispatches_of(Function<bool(ContextKey const&)> const&);
    bool may_see(Web::Extensions::ExtensionDescriptor const&, URL::URL const& url, Optional<URL::URL> const& document_url, bool is_private) const;

    u64 m_next_dispatch_id { 0 };

    // A request waiting on its listeners, and the dispatches whose answers it waits for.
    struct PendingDecision {
        int client_id { 0 };
        u64 request_id { 0 };
        Requests::WebRequestEvent event { Requests::WebRequestEvent::BeforeRequest };
        size_t outstanding { 0 };
        Vector<ExtensionAnswer> answers;
        Optional<URL::URL> initiator;
    };
    struct PendingDispatch {
        u64 decision_id { 0 };
        ContextKey context;
        String extension_id;
    };
    HashMap<u64, PendingDecision> m_pending_decisions;
    HashMap<u64, PendingDispatch> m_pending_dispatches;
    u64 m_next_decision_id { 0 };

    HashMap<ContextKey, ContextListeners, ContextKeyTraits> m_contexts;
};

}
