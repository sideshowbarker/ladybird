/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibRequests/WebRequest.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

namespace Web::Extensions {

using Requests::WebRequestEvent;

// The event's name as extensions see it: "onBeforeRequest", e.g.
WEBCOMMON_API StringView web_request_event_name(WebRequestEvent);
WEBCOMMON_API Optional<WebRequestEvent> web_request_event_from_name(StringView);

// The request types extensions can filter on, in the order browser.webRequest.ResourceType lists them.
WEBCOMMON_API ReadonlySpan<StringView> web_request_resource_types();

// One webRequest listener of one extension context, as the UI process needs it to decide which requests the listener
// hears and what it may change.
struct WEBCOMMON_API WebRequestListener {
    u64 id { 0 };
    WebRequestEvent event { WebRequestEvent::BeforeRequest };
    Vector<MatchPattern> urls;
    // Empty for every type.
    Vector<String> types;
    Optional<i64> tab_id;
    Optional<i64> window_id;
    bool blocking { false };
    bool request_headers { false };
    bool response_headers { false };

    bool operator==(WebRequestListener const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::WebRequestListener const&);

template<>
WEBCOMMON_API ErrorOr<Web::Extensions::WebRequestListener> decode(Decoder&);

}
