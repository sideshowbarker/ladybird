/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/WebRequest.h>
#include <LibURL/URL.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::WebRequestInterest const& interest)
{
    TRY(encoder.encode(interest.event));
    TRY(encoder.encode(interest.urls));
    TRY(encoder.encode(interest.types));
    TRY(encoder.encode(interest.blocking));
    return {};
}

template<>
ErrorOr<Requests::WebRequestInterest> decode(Decoder& decoder)
{
    Requests::WebRequestInterest interest;
    interest.event = TRY(decoder.decode<Requests::WebRequestEvent>());
    interest.urls = TRY(decoder.decode<Vector<String>>());
    interest.types = TRY(decoder.decode<Vector<String>>());
    interest.blocking = TRY(decoder.decode<bool>());
    return interest;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::WebRequestEventDetails const& details)
{
    TRY(encoder.encode(details.event));
    TRY(encoder.encode(details.url));
    TRY(encoder.encode(details.method));
    TRY(encoder.encode(details.context));
    TRY(encoder.encode(details.status_code));
    TRY(encoder.encode(details.status_line));
    TRY(encoder.encode(details.ip));
    TRY(encoder.encode(details.from_cache));
    TRY(encoder.encode(details.request_headers));
    TRY(encoder.encode(details.response_headers));
    TRY(encoder.encode(details.redirect_url));
    TRY(encoder.encode(details.error));
    return {};
}

template<>
ErrorOr<Requests::WebRequestEventDetails> decode(Decoder& decoder)
{
    Requests::WebRequestEventDetails details;
    details.event = TRY(decoder.decode<Requests::WebRequestEvent>());
    details.url = TRY(decoder.decode<URL::URL>());
    details.method = TRY(decoder.decode<ByteString>());
    details.context = TRY(decoder.decode<Requests::WebRequestContext>());
    details.status_code = TRY(decoder.decode<Optional<u32>>());
    details.status_line = TRY(decoder.decode<Optional<ByteString>>());
    details.ip = TRY(decoder.decode<Optional<ByteString>>());
    details.from_cache = TRY(decoder.decode<bool>());
    details.request_headers = TRY(decoder.decode<Optional<Vector<HTTP::Header>>>());
    details.response_headers = TRY(decoder.decode<Optional<Vector<HTTP::Header>>>());
    details.redirect_url = TRY(decoder.decode<Optional<URL::URL>>());
    details.error = TRY(decoder.decode<Optional<ByteString>>());
    return details;
}

}
