/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace regex {

class ECMAScriptRegex;

}

namespace Web::Extensions {

// One of the URL filters an extension event's listener may give, matching a URL when every condition it names holds.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/events/UrlFilter
class WEBCOMMON_API UrlFilter {
public:
    static ErrorOr<UrlFilter, String> parse(JsonObject const&);

    UrlFilter(UrlFilter&&);
    UrlFilter& operator=(UrlFilter&&);
    ~UrlFilter();

    [[nodiscard]] bool matches(URL::URL const&) const;

private:
    UrlFilter();

    // A condition on one part of the URL: the host, path, query, or the whole URL.
    struct Condition {
        enum class Part : u8 {
            Host,
            Path,
            Query,
            Url,
        };
        enum class Test : u8 {
            Contains,
            Equals,
            Prefix,
            Suffix,
        };
        Part part;
        Test test;
        String value;
    };

    Vector<Condition> m_conditions;
    Optional<Vector<String>> m_schemes;
    Optional<Vector<Variant<u16, Array<u16, 2>>>> m_ports;
    OwnPtr<regex::ECMAScriptRegex> m_url_matches;
    OwnPtr<regex::ECMAScriptRegex> m_origin_and_path_matches;
};

}
