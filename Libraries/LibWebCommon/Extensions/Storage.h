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
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

struct StorageRequest {
    enum class Operation : u8 { Get,
        Set,
        Remove,
        Clear,
        BytesInUse,
        // storage.session.setAccessLevel(), whose level is the items.
        SetAccessLevel };
    enum class Area : u8 { Local,
        Session,
        Sync,
        // An administrator's policy sets storage.managed, so extensions only read it, and here it holds nothing.
        Managed };
    Operation operation { Operation::Get };
    Optional<Vector<String>> keys;
    String items_json;
    Area area { Area::Local };
    bool operator==(StorageRequest const&) const = default;
};

// The name an area has in the API and in storage.onChanged: "local", "session", or "sync".
WEBCOMMON_API StringView storage_area_name(StorageRequest::Area);

struct StorageResult {
    enum class Type : u8 { Items,
        Bytes,
        Empty,
        Error };
    Type type { Type::Empty };
    String json_or_error;
    u64 bytes { 0 };
    bool operator==(StorageResult const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::StorageRequest const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::StorageRequest> decode(Decoder&);
template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::StorageResult const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::StorageResult> decode(Decoder&);

}
