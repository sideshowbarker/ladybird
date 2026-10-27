/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/Injection.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::ExtensionInjection const& injection)
{
    TRY(encoder.encode(injection.js_paths));
    TRY(encoder.encode(injection.js_sources));
    TRY(encoder.encode(injection.function_source));
    TRY(encoder.encode(injection.arguments_json));
    TRY(encoder.encode(to_underlying(injection.world)));
    return encoder.encode(injection.run_at);
}

template<>
ErrorOr<Web::Extensions::ExtensionInjection> decode(Decoder& decoder)
{
    Web::Extensions::ExtensionInjection injection;
    injection.js_paths = TRY(decoder.decode<Vector<String>>());
    injection.js_sources = TRY(decoder.decode<Vector<String>>());
    injection.function_source = TRY(decoder.decode<Optional<String>>());
    injection.arguments_json = TRY(decoder.decode<String>());
    auto world = TRY(decoder.decode<u8>());
    if (world > to_underlying(Web::Extensions::ExecutionWorld::Main))
        return Error::from_string_literal("Invalid extension injection world");
    injection.world = static_cast<Web::Extensions::ExecutionWorld>(world);
    injection.run_at = TRY(decoder.decode<Optional<Web::Extensions::RunAt>>());
    if (injection.js_paths.size() != injection.js_sources.size())
        return Error::from_string_literal("Mismatched extension injection sources");
    return injection;
}

}
