/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/WebRequestContext.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::WebRequestNavigable const& navigable)
{
    TRY(encoder.encode(navigable.namespace_id));
    TRY(encoder.encode(navigable.local_id));
    return {};
}

template<>
ErrorOr<Requests::WebRequestNavigable> decode(Decoder& decoder)
{
    Requests::WebRequestNavigable navigable;
    navigable.namespace_id = TRY(decoder.decode<u64>());
    navigable.local_id = TRY(decoder.decode<u64>());
    return navigable;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::WebRequestContext const& context)
{
    TRY(encoder.encode(context.serial));
    TRY(encoder.encode(context.type));
    TRY(encoder.encode(context.document_url));
    TRY(encoder.encode(context.origin_url));
    TRY(encoder.encode(context.page_id));
    TRY(encoder.encode(context.navigable));
    TRY(encoder.encode(context.is_navigation));
    return {};
}

template<>
ErrorOr<Requests::WebRequestContext> decode(Decoder& decoder)
{
    Requests::WebRequestContext context;
    context.serial = TRY(decoder.decode<u64>());
    context.type = TRY(decoder.decode<String>());
    context.document_url = TRY(decoder.decode<Optional<URL::URL>>());
    context.origin_url = TRY(decoder.decode<Optional<URL::URL>>());
    context.page_id = TRY(decoder.decode<u64>());
    context.navigable = TRY(decoder.decode<Optional<Requests::WebRequestNavigable>>());
    context.is_navigation = TRY(decoder.decode<bool>());
    return context;
}

}
