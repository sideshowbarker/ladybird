/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/WebRequest.h>

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

}
