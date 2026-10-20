/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/MessageSender.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::MessageSender const& sender)
{
    TRY(encoder.encode(sender.extension_id));
    TRY(encoder.encode(sender.url));
    TRY(encoder.encode(sender.from_background));
    return {};
}

template<>
ErrorOr<Web::Extensions::MessageSender> decode(Decoder& decoder)
{
    Web::Extensions::MessageSender sender;
    sender.extension_id = TRY(decoder.decode<String>());
    sender.url = TRY(decoder.decode<Optional<String>>());
    sender.from_background = TRY(decoder.decode<bool>());
    return sender;
}

}
