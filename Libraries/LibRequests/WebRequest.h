/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>

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

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::WebRequestInterest const&);

template<>
ErrorOr<Requests::WebRequestInterest> decode(Decoder&);

}
