/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Whether a content script's glob matches the whole of a serialized URL: * stands for any run of characters (none
// included), ? for exactly one, and everything else for itself.
WEBCOMMON_API bool url_glob_matches(StringView glob, StringView url);

// Whether a URL passes a content script's includeGlobs and excludeGlobs: With include globs, it has to match one of
// them, and it may match none of the exclude globs.
WEBCOMMON_API bool url_matches_globs(Vector<String> const& include_globs, Vector<String> const& exclude_globs, URL::URL const&);

}
