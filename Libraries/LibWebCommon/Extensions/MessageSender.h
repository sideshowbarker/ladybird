/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Who sent a runtime message: the extension it belongs to, the URL of the document it came from, and whether that
// document is the extension's background page.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/runtime/MessageSender
struct WEBCOMMON_API MessageSender {
    String extension_id;
    Optional<String> url;
    bool from_background { false };
    // The tab the sending document is in, as extensions see it, and its frame there: 0 for the tab's top-level document.
    Optional<String> tab_json {};
    Optional<i64> frame_id {};

    bool operator==(MessageSender const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::MessageSender const&);

template<>
WEBCOMMON_API ErrorOr<Web::Extensions::MessageSender> decode(Decoder&);

}
