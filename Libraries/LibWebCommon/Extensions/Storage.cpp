/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/Storage.h>

namespace Web::Extensions {

StringView storage_area_name(StorageRequest::Area area)
{
    switch (area) {
    case StorageRequest::Area::Local:
        return "local"sv;
    case StorageRequest::Area::Session:
        return "session"sv;
    case StorageRequest::Area::Sync:
        return "sync"sv;
    case StorageRequest::Area::Managed:
        return "managed"sv;
    }
    VERIFY_NOT_REACHED();
}

}

namespace IPC {

using Web::Extensions::StorageRequest;
using Web::Extensions::StorageResult;

template<>
ErrorOr<void> encode(Encoder& encoder, StorageRequest const& request)
{
    TRY(encoder.encode(to_underlying(request.operation)));
    TRY(encoder.encode(request.keys));
    TRY(encoder.encode(request.items_json));
    return encoder.encode(to_underlying(request.area));
}

template<>
ErrorOr<StorageRequest> decode(Decoder& decoder)
{
    auto operation = TRY(decoder.decode<u8>());
    if (operation > to_underlying(StorageRequest::Operation::SetAccessLevel))
        return Error::from_string_literal("Invalid extension storage operation");
    auto keys = TRY(decoder.decode<Optional<Vector<String>>>());
    auto items_json = TRY(decoder.decode<String>());
    auto area = TRY(decoder.decode<u8>());
    if (area > to_underlying(StorageRequest::Area::Managed))
        return Error::from_string_literal("Invalid extension storage area");
    return StorageRequest { static_cast<StorageRequest::Operation>(operation), move(keys), move(items_json), static_cast<StorageRequest::Area>(area) };
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
