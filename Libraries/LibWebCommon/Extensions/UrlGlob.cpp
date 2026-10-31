/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/Extensions/UrlGlob.h>

namespace Web::Extensions {

bool url_glob_matches(StringView glob, StringView url)
{
    // Each * first matches nothing, and takes one more character each time what follows it fails to match, so the
    // match only ever backs up to the most recent *.
    size_t glob_index = 0;
    size_t url_index = 0;
    Optional<size_t> star_index;
    size_t star_url_index = 0;
    while (url_index < url.length()) {
        if (glob_index < glob.length() && (glob[glob_index] == '?' || glob[glob_index] == url[url_index])) {
            ++glob_index;
            ++url_index;
        } else if (glob_index < glob.length() && glob[glob_index] == '*') {
            star_index = glob_index++;
            star_url_index = url_index;
        } else if (star_index.has_value()) {
            glob_index = *star_index + 1;
            url_index = ++star_url_index;
        } else {
            return false;
        }
    }
    while (glob_index < glob.length() && glob[glob_index] == '*')
        ++glob_index;
    return glob_index == glob.length();
}

bool url_matches_globs(Vector<String> const& include_globs, Vector<String> const& exclude_globs, URL::URL const& url)
{
    if (include_globs.is_empty() && exclude_globs.is_empty())
        return true;
    auto serialized = url.serialize();
    auto matches = [&](String const& glob) { return url_glob_matches(glob, serialized); };
    if (!include_globs.is_empty() && !include_globs.first_matching(matches).has_value())
        return false;
    return !exclude_globs.first_matching(matches).has_value();
}

}
