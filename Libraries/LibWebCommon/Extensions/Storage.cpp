/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/Storage.h>

namespace IPC {

using Web::Extensions::StorageRequest;
using Web::Extensions::StorageResult;

template<>
ErrorOr<void> encode(Encoder& encoder, StorageRequest const& request)
{
    TRY(encoder.encode(to_underlying(request.operation)));
    TRY(encoder.encode(request.keys));
    return encoder.encode(request.items_json);
}

template<>
ErrorOr<StorageRequest> decode(Decoder& decoder)
{
    auto operation = TRY(decoder.decode<u8>());
    if (operation > to_underlying(StorageRequest::Operation::BytesInUse))
        return Error::from_string_literal("Invalid extension storage operation");
    return StorageRequest { static_cast<StorageRequest::Operation>(operation), TRY(decoder.decode<Optional<Vector<String>>>()), TRY(decoder.decode<String>()) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, StorageResult const& result)
{
    TRY(encoder.encode(to_underlying(result.type)));
    TRY(encoder.encode(result.json_or_error));
    return encoder.encode(result.bytes);
}

template<>
ErrorOr<StorageResult> decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<u8>());
    if (type > to_underlying(StorageResult::Type::Error))
        return Error::from_string_literal("Invalid extension storage result");
    return StorageResult { static_cast<StorageResult::Type>(type), TRY(decoder.decode<String>()), TRY(decoder.decode<u64>()) };
}

}
