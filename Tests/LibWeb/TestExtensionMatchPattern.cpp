/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/MatchPattern.h>

using Web::Extensions::MatchPattern;

static MatchPattern parse(StringView pattern)
{
    auto result = MatchPattern::parse(pattern);
    VERIFY(!result.is_error());
    return result.release_value();
}

static bool matches(StringView pattern, StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parse(pattern).matches(*parsed_url);
}

TEST_CASE(parses_all_urls)
{
    auto pattern = parse("<all_urls>"sv);
    EXPECT(pattern.matches_all_urls());
    EXPECT(matches("<all_urls>"sv, "https://a.b/c?d"sv));
    EXPECT(matches("<all_urls>"sv, "http://a.b/"sv));
    EXPECT(matches("<all_urls>"sv, "file:///x"sv));
    EXPECT(matches("<all_urls>"sv, "ws://h/"sv));
    EXPECT(matches("<all_urls>"sv, "ftp://h/"sv));
    EXPECT(!matches("<all_urls>"sv, "data:text/plain,x"sv));
    EXPECT(!matches("<all_urls>"sv, "ladybird-extension://id/"sv));
    EXPECT(!matches("<all_urls>"sv, "about:blank"sv));
}

TEST_CASE(star_scheme_is_http_https_ws_wss)
{
    EXPECT(matches("*://*/*"sv, "http://e.com/"sv));
    EXPECT(matches("*://*/*"sv, "https://e.com/"sv));
    EXPECT(matches("*://*/*"sv, "ws://e.com/"sv));
    EXPECT(matches("*://*/*"sv, "wss://e.com/"sv));
    EXPECT(!matches("*://*/*"sv, "file:///e"sv));
    EXPECT(!matches("*://*/*"sv, "ftp://e.com/"sv));
    EXPECT(!parse("*://*/*"sv).matches_all_urls());
}

TEST_CASE(host_rules)
{
    EXPECT(matches("*://*.example.com/*"sv, "https://example.com/"sv));
    EXPECT(matches("*://*.example.com/*"sv, "https://a.example.com/"sv));
    EXPECT(matches("*://*.example.com/*"sv, "https://a.b.example.com/"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "https://notexample.com/"sv));
    EXPECT(!matches("*://*.example.com/*"sv, "https://example.com.evil/"sv));
    EXPECT(matches("*://example.com/*"sv, "https://example.com/"sv));
    EXPECT(!matches("*://example.com/*"sv, "https://www.example.com/"sv));
    EXPECT(matches("*://EXAMPLE.com/*"sv, "https://example.COM/"sv));
    EXPECT(matches("*://*/*"sv, "https://anything.at.all/"sv));
}

TEST_CASE(path_rules)
{
    EXPECT(matches("https://e.com/foo*"sv, "https://e.com/foo"sv));
    EXPECT(matches("https://e.com/foo*"sv, "https://e.com/foobar"sv));
    EXPECT(matches("https://e.com/foo*"sv, "https://e.com/foo/bar?x=1"sv));
    EXPECT(!matches("https://e.com/foo*"sv, "https://e.com/fo"sv));
    EXPECT(matches("https://e.com/*/bar"sv, "https://e.com/a/bar"sv));
    EXPECT(matches("https://e.com/*/bar"sv, "https://e.com/a/b/bar"sv));
    EXPECT(!matches("https://e.com/*/bar"sv, "https://e.com/bar"sv));
    EXPECT(matches("https://e.com/"sv, "https://e.com/"sv));
    EXPECT(!matches("https://e.com/"sv, "https://e.com/x"sv));
    EXPECT(matches("https://e.com/*"sv, "https://e.com/"sv));
    // The query string is part of the path match.
    EXPECT(matches("https://e.com/foo?x=*"sv, "https://e.com/foo?x=1"sv));
    EXPECT(!matches("https://e.com/foo?x=*"sv, "https://e.com/foo"sv));
    EXPECT(!matches("https://e.com/foo"sv, "https://e.com/foo?x=1"sv));
    // The fragment is not.
    EXPECT(matches("https://e.com/foo"sv, "https://e.com/foo#frag"sv));
}

TEST_CASE(file_scheme_has_empty_host)
{
    EXPECT(matches("file:///*"sv, "file:///etc/hosts"sv));
    EXPECT(matches("file:///etc/*"sv, "file:///etc/hosts"sv));
    EXPECT(!matches("file:///etc/*"sv, "file:///usr/bin"sv));
    EXPECT(MatchPattern::parse("file://*/x"sv).is_error());
    EXPECT(MatchPattern::parse("file://host/x"sv).is_error());
}

TEST_CASE(invalid_patterns)
{
    EXPECT(MatchPattern::parse("http://"sv).is_error());
    EXPECT(MatchPattern::parse("http:///"sv).is_error());
    EXPECT(MatchPattern::parse("*://"sv).is_error());
    EXPECT(MatchPattern::parse("https://*foo/"sv).is_error());
    EXPECT(MatchPattern::parse("https://foo.*/"sv).is_error());
    EXPECT(MatchPattern::parse("https://*.*/"sv).is_error());
    EXPECT(MatchPattern::parse("foo://bar/"sv).is_error());
    EXPECT(MatchPattern::parse("https://e.com"sv).is_error());
    EXPECT(MatchPattern::parse("<all_urls>x"sv).is_error());
    EXPECT(MatchPattern::parse(""sv).is_error());
    EXPECT(MatchPattern::parse("e.com/*"sv).is_error());
}

TEST_CASE(port_is_ignored)
{
    EXPECT(matches("https://e.com/*"sv, "https://e.com:8443/"sv));
    EXPECT(matches("http://localhost/*"sv, "http://localhost:8000/x"sv));
}

TEST_CASE(accessors_and_round_trip)
{
    auto pattern = parse("*://*.example.com/a/*"sv);
    EXPECT_EQ(pattern.scheme(), "*"sv);
    EXPECT_EQ(pattern.host(), "*.example.com"sv);
    EXPECT_EQ(pattern.path(), "/a/*"sv);
    EXPECT_EQ(pattern.to_string(), "*://*.example.com/a/*"sv);
    EXPECT_EQ(parse(pattern.to_string()), pattern);

    auto all_urls = parse("<all_urls>"sv);
    EXPECT_EQ(all_urls.to_string(), "<all_urls>"sv);
    EXPECT_EQ(parse(all_urls.to_string()), all_urls);
    EXPECT_EQ(MatchPattern::all_urls(), all_urls);

    auto file = parse("file:///tmp/*"sv);
    EXPECT_EQ(file.host(), ""sv);
    EXPECT_EQ(file.to_string(), "file:///tmp/*"sv);
}

TEST_CASE(hosts_follow_url_validation_and_canonicalization)
{
    for (auto pattern : {
             "https://bad host/*"sv,
             "https://bad\thost/*"sv,
             "https://bad\nhost/*"sv,
             "https://user@example.com/*"sv,
             "https://host?query/*"sv,
             "https://host#fragment/*"sv,
             "https://host\\path/*"sv,
             "https://host%2fpath/*"sv,
             "https://host%00/*"sv,
             "https://[:::1]/*"sv,
             "https://[]/*"sv,
             "https://[::1/*"sv,
             "https://[::1]:80/*"sv,
             "https://%2a.example.com/*"sv,
         }) {
        EXPECT(MatchPattern::parse(pattern).is_error());
    }
    EXPECT(matches("https://[0:0:0:0:0:0:0:1]/*"sv, "https://[::1]/path"sv));
    EXPECT_EQ(parse("https://127.1/*"sv).host(), "127.0.0.1"sv);
    EXPECT_EQ(parse("https://EXAMPLE%2eCOM/*"sv).host(), "example.com"sv);
    EXPECT(matches("https://*.BÜCHER.example/*"sv, "https://a.xn--bcher-kva.example/"sv));
}
