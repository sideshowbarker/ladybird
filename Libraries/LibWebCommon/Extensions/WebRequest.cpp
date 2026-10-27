/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/WebRequest.h>

namespace Web::Extensions {

static constexpr Array web_request_event_names {
    "onBeforeRequest"sv,
    "onBeforeSendHeaders"sv,
    "onSendHeaders"sv,
    "onHeadersReceived"sv,
    "onAuthRequired"sv,
    "onBeforeRedirect"sv,
    "onResponseStarted"sv,
    "onCompleted"sv,
    "onErrorOccurred"sv,
};

StringView web_request_event_name(WebRequestEvent event)
{
    return web_request_event_names[to_underlying(event)];
}

Optional<WebRequestEvent> web_request_event_from_name(StringView name)
{
    for (size_t i = 0; i < web_request_event_names.size(); ++i) {
        if (web_request_event_names[i] == name)
            return static_cast<WebRequestEvent>(i);
    }
    return {};
}

// Extensions compare these values, so they're fixed.
static constexpr Array resource_types {
    "main_frame"sv,
    "sub_frame"sv,
    "stylesheet"sv,
    "script"sv,
    "image"sv,
    "object"sv,
    "xmlhttprequest"sv,
    "xslt"sv,
    "ping"sv,
    "beacon"sv,
    "xml_dtd"sv,
    "font"sv,
    "media"sv,
    "websocket"sv,
    "csp_report"sv,
    "imageset"sv,
    "web_manifest"sv,
    "speculative"sv,
    "json"sv,
    "other"sv,
};

ReadonlySpan<StringView> web_request_resource_types()
{
    return resource_types.span();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::WebRequestListener const& listener)
{
    TRY(encoder.encode(listener.id));
    TRY(encoder.encode(listener.event));
    TRY(encoder.encode(listener.urls));
    TRY(encoder.encode(listener.types));
    TRY(encoder.encode(listener.tab_id));
    TRY(encoder.encode(listener.window_id));
    TRY(encoder.encode(listener.blocking));
    TRY(encoder.encode(listener.request_headers));
    TRY(encoder.encode(listener.response_headers));
    return {};
}

template<>
ErrorOr<Web::Extensions::WebRequestListener> decode(Decoder& decoder)
{
    Web::Extensions::WebRequestListener listener;
    listener.id = TRY(decoder.decode<u64>());
    listener.event = TRY(decoder.decode<Web::Extensions::WebRequestEvent>());
    listener.urls = TRY(decoder.decode<Vector<Web::Extensions::MatchPattern>>());
    listener.types = TRY(decoder.decode<Vector<String>>());
    listener.tab_id = TRY(decoder.decode<Optional<i64>>());
    listener.window_id = TRY(decoder.decode<Optional<i64>>());
    listener.blocking = TRY(decoder.decode<bool>());
    listener.request_headers = TRY(decoder.decode<bool>());
    listener.response_headers = TRY(decoder.decode<bool>());
    return listener;
}

}
