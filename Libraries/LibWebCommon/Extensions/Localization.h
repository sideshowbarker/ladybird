/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

enum class UnknownMessage { Fail,
    Keep };

WEBCOMMON_API bool is_valid_message_name(StringView);
WEBCOMMON_API ErrorOr<HashMap<String, String>, String> parse_message_catalog(StringView json);
WEBCOMMON_API HashMap<String, String> built_in_messages(StringView extension_id, StringView ui_locale_underscored);
WEBCOMMON_API ErrorOr<ByteString, String> substitute_message_tokens(StringView, HashMap<String, String> const&, UnknownMessage);
WEBCOMMON_API Optional<String> format_message(StringView, ReadonlySpan<String> substitutions, bool escape_lt);

}
