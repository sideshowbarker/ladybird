/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/MessageResult.h>

namespace Web::Extensions {

MessageResult MessageResult::no_receiver()
{
    return Error { "Could not establish connection. Receiving end does not exist."_string };
}

}

namespace IPC {

using Web::Extensions::MessageResult;

template<>
ErrorOr<void> encode(Encoder& encoder, MessageResult const& result)
{
    return result.value.visit(
        [&](MessageResult::Reply const& reply) -> ErrorOr<void> { TRY(encoder.encode(u8 { 0 })); return encoder.encode(reply.json); },
        [&](MessageResult::NoReply) { return encoder.encode(u8 { 1 }); },
        [&](MessageResult::NoListener) { return encoder.encode(u8 { 2 }); },
        [&](MessageResult::Error const& error) -> ErrorOr<void> { TRY(encoder.encode(u8 { 3 })); return encoder.encode(error.message); });
}

template<>
ErrorOr<MessageResult> decode(Decoder& decoder)
{
    switch (TRY(decoder.decode<u8>())) {
    case 0:
        return MessageResult::Reply { TRY(decoder.decode<String>()) };
    case 1:
        return MessageResult::NoReply {};
    case 2:
        return MessageResult::NoListener {};
    case 3:
        return MessageResult::Error { TRY(decoder.decode<String>()) };
    default:
        return Error::from_string_literal("Invalid extension message result");
    }
}

}
