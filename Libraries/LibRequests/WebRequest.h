/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibIPC/Forward.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/WebRequestContext.h>
#include <LibURL/URL.h>

namespace Requests {

// The webRequest events extensions listen to, in the order a request that succeeds without a redirect reaches them.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/webRequest
enum class WebRequestEvent : u8 {
    BeforeRequest,
    BeforeSendHeaders,
    SendHeaders,
    HeadersReceived,
    AuthRequired,
    BeforeRedirect,
    ResponseStarted,
    Completed,
    ErrorOccurred,
};

// Which requests some extension listens to for one event: those whose URL one of the patterns matches and whose type
// is listed (any type when none is), and whether any such listener may change them.
struct WebRequestInterest {
    WebRequestEvent event { WebRequestEvent::BeforeRequest };
    Vector<String> urls;
    Vector<String> types;
    bool blocking { false };

    bool operator==(WebRequestInterest const&) const = default;
};

// One event of one request, as RequestServer saw it.
struct WebRequestEventDetails {
    WebRequestEvent event { WebRequestEvent::BeforeRequest };
    URL::URL url;
    ByteString method;
    WebRequestContext context;
    // A WebSocket's id is its own, apart from the ids of the client's other requests.
    bool is_websocket { false };
    Optional<u32> status_code;
    // "HTTP/1.1 200 OK", e.g.
    Optional<ByteString> status_line;
    // The address the response came from, when it came over the network.
    Optional<ByteString> ip;
    bool from_cache { false };
    // The headers as they went out, for the events that report them.
    Optional<Vector<HTTP::Header>> request_headers;
    Optional<Vector<HTTP::Header>> response_headers;
    Optional<URL::URL> redirect_url;
    // The name extensions compare an error with: "NS_ERROR_UNKNOWN_HOST", e.g.
    Optional<ByteString> error;
};

// What extensions' blocking listeners decided about one event of a request: to cancel it, to send it somewhere else, or
// to replace the headers it sends or the ones its response has. Nothing set leaves the request as it is.
struct WebRequestDecision {
    bool cancel { false };
    Optional<URL::URL> redirect_url;
    Optional<Vector<HTTP::Header>> request_headers;
    Optional<Vector<HTTP::Header>> response_headers;
};

// Extensions compare these names, so they're fixed.
constexpr StringView web_request_error_name(NetworkError error)
{
    switch (error) {
    case NetworkError::UnableToResolveProxy:
        return "NS_ERROR_UNKNOWN_PROXY_HOST"sv;
    case NetworkError::UnableToResolveHost:
        return "NS_ERROR_UNKNOWN_HOST"sv;
    case NetworkError::UnableToConnect:
        return "NS_ERROR_CONNECTION_REFUSED"sv;
    case NetworkError::TimeoutReached:
        return "NS_ERROR_NET_TIMEOUT"sv;
    case NetworkError::TooManyRedirects:
        return "NS_ERROR_REDIRECT_LOOP"sv;
    case NetworkError::MalformedUrl:
        return "NS_ERROR_MALFORMED_URI"sv;
    case NetworkError::IncompleteContent:
        return "NS_ERROR_NET_INTERRUPT"sv;
    case NetworkError::InvalidContentEncoding:
        return "NS_ERROR_INVALID_CONTENT_ENCODING"sv;
    case NetworkError::BlockedByExtension:
        return "NS_ERROR_ABORT"sv;
    case NetworkError::SSLHandshakeFailed:
    case NetworkError::SSLVerificationFailed:
    case NetworkError::RequestServerDied:
    case NetworkError::CacheReadFailed:
    case NetworkError::Unknown:
        return "NS_ERROR_FAILURE"sv;
    }
    VERIFY_NOT_REACHED();
}

// The error of a request its page stopped waiting for.
constexpr StringView web_request_aborted_error_name = "NS_BINDING_ABORTED"sv;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestInterest const&);

template<>
ErrorOr<Requests::WebRequestInterest> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestDecision const&);

template<>
ErrorOr<Requests::WebRequestDecision> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestEventDetails const&);

template<>
ErrorOr<Requests::WebRequestEventDetails> decode(Decoder&);

}
