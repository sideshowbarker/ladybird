/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/Scheme.h>

namespace Web::Extensions {

static constexpr auto all_urls_pattern = "<all_urls>"sv;

// The schemes a pattern may name. "*" stands for the first four.
static constexpr Array star_schemes { "http"sv, "https"sv, "ws"sv, "wss"sv };
static constexpr Array supported_schemes { "http"sv, "https"sv, "ws"sv, "wss"sv, "file"sv, "ftp"sv };

static bool is_supported_scheme(StringView scheme)
{
    return supported_schemes.contains_slow(scheme);
}

bool glob_matches(StringView glob, StringView text)
{
    size_t glob_index = 0;
    size_t text_index = 0;
    Optional<size_t> star_glob_index;
    size_t star_text_index = 0;

    while (text_index < text.length()) {
        if (glob_index < glob.length() && glob[glob_index] == '*') {
            star_glob_index = glob_index++;
            star_text_index = text_index;
            continue;
        }
        if (glob_index < glob.length() && glob[glob_index] == text[text_index]) {
            ++glob_index;
            ++text_index;
            continue;
        }
        if (star_glob_index.has_value()) {
            glob_index = *star_glob_index + 1;
            text_index = ++star_text_index;
            continue;
        }
        return false;
    }

    while (glob_index < glob.length() && glob[glob_index] == '*')
        ++glob_index;
    return glob_index == glob.length();
}

MatchPattern::MatchPattern(String scheme, String host, String path)
    : m_scheme(move(scheme))
    , m_host(move(host))
    , m_path(move(path))
{
}

MatchPattern MatchPattern::all_urls()
{
    MatchPattern pattern;
    pattern.m_matches_all_urls = true;
    return pattern;
}

ErrorOr<MatchPattern> MatchPattern::parse(StringView pattern, AllowExtensionScheme allow_extension_scheme)
{
    if (pattern == all_urls_pattern)
        return all_urls();

    auto scheme_end = pattern.find("://"sv);
    if (!scheme_end.has_value())
        return Error::from_string_literal("A match pattern needs a scheme followed by \"://\"");

    auto scheme = pattern.substring_view(0, *scheme_end);
    if (scheme != "*"sv && !is_supported_scheme(scheme) && !(allow_extension_scheme == AllowExtensionScheme::Yes && scheme == Web::Extensions::scheme))
        return Error::from_string_literal("A match pattern's scheme must be *, http, https, ws, wss, file, or ftp");

    auto rest = pattern.substring_view(*scheme_end + 3);
    auto path_start = rest.find('/');
    if (!path_start.has_value())
        return Error::from_string_literal("A match pattern needs a path that begins with \"/\"");

    auto host = rest.substring_view(0, *path_start);
    auto path = rest.substring_view(*path_start);
    auto canonical_host = TRY(String::from_utf8(host));

    if (scheme == "file"sv) {
        if (!host.is_empty())
            return Error::from_string_literal("A file match pattern's host must be empty");
    } else {
        if (host.is_empty())
            return Error::from_string_literal("A match pattern needs a host");
        if (host != "*"sv) {
            auto host_after_wildcard = host.starts_with("*."sv) ? host.substring_view(2) : host;
            if (host_after_wildcard.is_empty() || host_after_wildcard.contains('*'))
                return Error::from_string_literal("A match pattern's host may only use \"*\" on its own or as a \"*.\" prefix");
            auto parsed_host = URL::Parser::parse_host(host_after_wildcard);
            if (!parsed_host.has_value() || parsed_host->is_empty_host())
                return Error::from_string_literal("A match pattern needs a valid URL host");
            canonical_host = parsed_host->serialize();
            if (canonical_host.contains('*'))
                return Error::from_string_literal("A match pattern can't encode a host wildcard");
            if (host.starts_with("*."sv))
                canonical_host = TRY(String::formatted("*.{}", canonical_host));
        }
    }

    return MatchPattern {
        TRY(String::from_utf8(scheme)),
        move(canonical_host),
        TRY(String::from_utf8(path)),
    };
}

bool MatchPattern::scheme_matches(StringView url_scheme) const
{
    if (m_scheme == "*"sv)
        return star_schemes.contains_slow(url_scheme);
    return m_scheme == url_scheme;
}

bool MatchPattern::host_matches(StringView url_host) const
{
    if (m_scheme == "file"sv)
        return url_host.is_empty();
    if (m_host == "*"sv)
        return true;
    if (m_host.starts_with_bytes("*."sv)) {
        auto suffix = m_host.bytes_as_string_view().substring_view(2);
        if (url_host.equals_ignoring_ascii_case(suffix))
            return true;
        return url_host.length() > suffix.length()
            && url_host[url_host.length() - suffix.length() - 1] == '.'
            && url_host.substring_view(url_host.length() - suffix.length()).equals_ignoring_ascii_case(suffix);
    }
    return url_host.equals_ignoring_ascii_case(m_host);
}

bool MatchPattern::path_matches(URL::URL const& url) const
{
    StringBuilder path_and_query;
    path_and_query.append(url.serialize_path());
    if (auto query = url.query(); query.has_value()) {
        path_and_query.append('?');
        path_and_query.append(*query);
    }
    return glob_matches(m_path, path_and_query.string_view());
}

bool MatchPattern::matches(URL::URL const& url) const
{
    if (m_matches_all_urls)
        return is_supported_scheme(url.scheme());
    return scheme_matches(url.scheme()) && host_matches(url.serialized_host()) && path_matches(url);
}

bool MatchPattern::contains(MatchPattern const& other) const
{
    if (m_matches_all_urls)
        return true;
    if (other.m_matches_all_urls)
        return false;
    if (m_scheme != other.m_scheme && !(m_scheme == "*"sv && star_schemes.contains_slow(other.m_scheme.bytes_as_string_view())))
        return false;
    if (m_scheme == "file"sv)
        return glob_matches(m_path, other.m_path);
    // A wildcard host covers only what this pattern's host covers: "*" covers any, and "*.a.b" covers "*.c.a.b".
    if (m_host != "*"sv) {
        if (other.m_host == "*"sv)
            return false;
        auto other_host = other.m_host.bytes_as_string_view();
        if (other_host.starts_with("*."sv)) {
            if (!m_host.starts_with_bytes("*."sv))
                return false;
            other_host = other_host.substring_view(2);
        }
        if (!host_matches(other_host))
            return false;
    }
    return glob_matches(m_path, other.m_path);
}

String MatchPattern::to_string() const
{
    if (m_matches_all_urls)
        return String::from_utf8_without_validation(all_urls_pattern.bytes());
    return MUST(String::formatted("{}://{}{}", m_scheme, m_host, m_path));
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Extensions::MatchPattern const& pattern)
{
    return encoder.encode(pattern.to_string());
}

template<>
ErrorOr<Web::Extensions::MatchPattern> decode(Decoder& decoder)
{
    auto pattern = TRY(decoder.decode<String>());
    return Web::Extensions::MatchPattern::parse(pattern);
}

}
