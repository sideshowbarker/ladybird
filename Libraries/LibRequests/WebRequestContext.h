/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>

namespace Requests {

// A navigable's cross-process id, as the UI process knows it.
struct WebRequestNavigable {
    u64 namespace_id { 0 };
    u64 local_id { 0 };

    bool operator==(WebRequestNavigable const&) const = default;
};

// What a page's process knows about one of its requests that extensions' webRequest listeners ask about. It goes with
// the request only while extensions are installed, and RequestServer reports a request only when it has one.
struct WebRequestContext {
    // The same for every hop of a request that redirects; unique within the process that made it.
    u64 serial { 0 };
    // The webRequest type: "main_frame", "script", e.g.
    String type;
    // The document whose fetch client made the request; absent for a navigation, whose document is its target's parent.
    Optional<URL::URL> document_url;
    // The document that caused the request.
    Optional<URL::URL> origin_url;
    u64 page_id { 0 };
    // The navigable a navigation loads into, or the one whose document made any other request.
    Optional<WebRequestNavigable> navigable;
    bool is_navigation { false };

    bool operator==(WebRequestContext const&) const = default;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestNavigable const&);

template<>
ErrorOr<Requests::WebRequestNavigable> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestContext const&);

template<>
ErrorOr<Requests::WebRequestContext> decode(Decoder&);

}
