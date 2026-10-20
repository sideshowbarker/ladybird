/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Decode a URL path into a relative file path, rejecting ambiguous separators and escapes above the directory.
WEBCOMMON_API ErrorOr<ByteString> canonical_resource_path(StringView url_path);

}
