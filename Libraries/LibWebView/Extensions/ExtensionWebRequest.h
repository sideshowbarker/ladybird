/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibRequests/WebRequest.h>
#include <LibWebCommon/Extensions/WebRequest.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The webRequest listeners of every extension context, by the page hosting the context, and the requests extensions
// have heard about. RequestServer is told which requests any listener wants, and asks about no others.
class WEBVIEW_API ExtensionWebRequestRouter {
    AK_ALLOC_WITH_KMALLOC;

public:
    // A page that hosts extension contexts.
    class Endpoint {
    public:
        virtual ~Endpoint() = default;
    };

    void listeners_changed(Endpoint&, String const& extension_id, u64 context_id, Vector<Web::Extensions::WebRequestListener>);
    void context_destroyed(Endpoint&, u64 context_id);
    void endpoint_destroyed(Endpoint&);

    // Every distinct filter some listener has, for RequestServer.
    Vector<Requests::WebRequestInterest> interest() const;
    Function<void()> on_interest_changed;

    // The requestId extensions see for a RequestServer client's request: the same for every hop of a request that
    // redirects, and never reused.
    String request_id_for(int client_id, u64 serial);
    void forget_request(int client_id, u64 serial);

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
    struct RequestKey {
        int client_id { 0 };
        u64 serial { 0 };

        bool operator==(RequestKey const&) const = default;
    };
    struct RequestKeyTraits : public DefaultTraits<RequestKey> {
        static unsigned hash(RequestKey const& key) { return pair_int_hash(u32_hash(static_cast<u32>(key.client_id)), u64_hash(key.serial)); }
    };

    void notify_interest_changed();

    HashMap<ContextKey, ContextListeners, ContextKeyTraits> m_contexts;
    HashMap<RequestKey, String, RequestKeyTraits> m_request_ids;
    u64 m_next_request_id { 0 };
};

}
