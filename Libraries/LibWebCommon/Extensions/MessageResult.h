/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

struct WEBCOMMON_API MessageResult {
    struct Reply {
        String json;
        bool operator==(Reply const&) const = default;
    };
    struct NoReply {
        bool operator==(NoReply const&) const = default;
    };
    struct NoListener {
        bool operator==(NoListener const&) const = default;
    };
    struct Error {
        String message;
        bool operator==(Error const&) const = default;
    };

    MessageResult() = default;
    MessageResult(Reply reply)
        : value(move(reply))
    {
    }
    MessageResult(NoReply reply)
        : value(reply)
    {
    }
    MessageResult(NoListener reply)
        : value(reply)
    {
    }
    MessageResult(Error error)
        : value(move(error))
    {
    }

    static MessageResult no_receiver();
    Variant<Reply, NoReply, NoListener, Error> value { NoReply {} };
    bool operator==(MessageResult const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::MessageResult const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::MessageResult> decode(Decoder&);

}
