/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ResourcePath.h>

namespace Web::Extensions {

ErrorOr<ByteString> canonical_resource_path(StringView url_path)
{
    if (!url_path.starts_with('/'))
        return Error::from_string_literal("An extension resource path must begin with \"/\"");
    if (url_path.contains("%2f"sv, CaseSensitivity::CaseInsensitive))
        return Error::from_string_literal("An extension resource path can't contain encoded separators");

    auto decoded = URL::percent_decode(url_path);
    if (decoded.contains('\\') || decoded.contains('\0'))
        return Error::from_string_literal("An extension resource path can't contain backslashes or NULs");

    Vector<StringView> segments;
    auto raw_segments = StringView { decoded }.split_view('/', SplitBehavior::KeepEmpty);
    for (size_t i = 1; i < raw_segments.size(); ++i) {
        auto segment = raw_segments[i];
        if (segment.is_empty())
            return Error::from_string_literal("An extension resource path can't contain empty segments");
        if (segment == "."sv)
            continue;
        if (segment == ".."sv) {
            if (segments.is_empty())
                return Error::from_string_literal("An extension resource path can't leave the extension directory");
            segments.take_last();
            continue;
        }
        segments.append(segment);
    }

    if (segments.is_empty())
        return Error::from_string_literal("An extension resource path must name a file");

    StringBuilder relative_path;
    relative_path.join('/', segments);
    return relative_path.to_byte_string();
}

}
