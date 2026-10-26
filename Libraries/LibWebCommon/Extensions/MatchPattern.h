/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Matches text against a glob whose only metacharacter is "*", which stands for any run of characters.
WEBCOMMON_API bool glob_matches(StringView glob, StringView text);

// A WebExtension match pattern: "<all_urls>" or "<scheme>://<host><path>", where the scheme is one of
// "*", "http", "https", "ws", "wss", "file", or "ftp" ("*" stands for http, https, ws, and wss); the host is
// "*", "*." followed by a host, or a host (and empty for "file"); and the path begins with "/" and may contain
// "*" wildcards. The path pattern is matched against the URL's path plus its query string.
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Match_patterns
class WEBCOMMON_API MatchPattern {
public:
    // A pattern an extension uses to find tabs may also name the extension scheme, so it can find its own pages.
    enum class AllowExtensionScheme : bool {
        No,
        Yes,
    };
    static ErrorOr<MatchPattern> parse(StringView pattern, AllowExtensionScheme = AllowExtensionScheme::No);
    static MatchPattern all_urls();

    [[nodiscard]] bool matches(URL::URL const&) const;
    [[nodiscard]] bool matches_all_urls() const { return m_matches_all_urls; }

    [[nodiscard]] String const& scheme() const { return m_scheme; }
    [[nodiscard]] String const& host() const { return m_host; }
    [[nodiscard]] String const& path() const { return m_path; }
    [[nodiscard]] String to_string() const;

    bool operator==(MatchPattern const&) const = default;

private:
    MatchPattern() = default;
    MatchPattern(String scheme, String host, String path);

    [[nodiscard]] bool scheme_matches(StringView url_scheme) const;
    [[nodiscard]] bool host_matches(StringView url_host) const;
    [[nodiscard]] bool path_matches(URL::URL const&) const;

    bool m_matches_all_urls { false };
    String m_scheme;
    String m_host;
    String m_path;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::MatchPattern const&);

template<>
WEBCOMMON_API ErrorOr<Web::Extensions::MatchPattern> decode(Decoder&);

}
