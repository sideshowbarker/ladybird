/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/UrlFilter.h>

using Web::Extensions::UrlFilter;

static UrlFilter filter(StringView json)
{
    auto object = JsonValue::from_string(json).release_value().as_object();
    return UrlFilter::parse(object).release_value();
}

static bool matches(StringView json, StringView url)
{
    return filter(json).matches(URL::Parser::basic_parse(url).release_value());
}

static String parse_error(StringView json)
{
    auto object = JsonValue::from_string(json).release_value().as_object();
    auto result = UrlFilter::parse(object);
    VERIFY(result.is_error());
    return result.release_error();
}

TEST_CASE(host_keys)
{
    // hostContains sees the host with a dot in front, so ".example" finds a whole label.
    EXPECT(matches(R"({ "hostContains": "xamp" })"sv, "https://www.example.com/"sv));
    EXPECT(matches(R"({ "hostContains": ".example" })"sv, "https://example.com/"sv));
    EXPECT(!matches(R"({ "hostContains": ".xample" })"sv, "https://example.com/"sv));
    EXPECT(matches(R"({ "hostEquals": "www.example.com" })"sv, "https://www.example.com/"sv));
    EXPECT(!matches(R"({ "hostEquals": "example.com" })"sv, "https://www.example.com/"sv));
    EXPECT(matches(R"({ "hostPrefix": "www." })"sv, "https://www.example.com/"sv));
    EXPECT(!matches(R"({ "hostPrefix": "www." })"sv, "https://example.com/"sv));
    EXPECT(matches(R"({ "hostSuffix": ".example.com" })"sv, "https://www.example.com/"sv));
    EXPECT(!matches(R"({ "hostSuffix": ".example.com" })"sv, "https://example.com/"sv));
}

TEST_CASE(path_and_query_keys)
{
    EXPECT(matches(R"({ "pathContains": "art" })"sv, "https://a.example/news/article?id=1"sv));
    EXPECT(matches(R"({ "pathEquals": "/news/article" })"sv, "https://a.example/news/article?id=1"sv));
    EXPECT(!matches(R"({ "pathEquals": "/news" })"sv, "https://a.example/news/article"sv));
    EXPECT(matches(R"({ "pathPrefix": "/news/" })"sv, "https://a.example/news/article"sv));
    EXPECT(matches(R"({ "pathSuffix": "article" })"sv, "https://a.example/news/article#top"sv));
    EXPECT(matches(R"({ "queryContains": "d=1" })"sv, "https://a.example/?id=1"sv));
    // A query is compared without its "?".
    EXPECT(matches(R"({ "queryEquals": "id=1" })"sv, "https://a.example/?id=1"sv));
    EXPECT(matches(R"({ "queryPrefix": "id" })"sv, "https://a.example/?id=1"sv));
    EXPECT(matches(R"({ "querySuffix": "=1" })"sv, "https://a.example/?id=1"sv));
    EXPECT(!matches(R"({ "queryContains": "id" })"sv, "https://a.example/"sv));
}

TEST_CASE(url_keys)
{
    EXPECT(matches(R"({ "urlContains": "example/x" })"sv, "https://a.example/x"sv));
    EXPECT(matches(R"({ "urlEquals": "https://a.example/x" })"sv, "https://a.example/x"sv));
    EXPECT(matches(R"({ "urlPrefix": "https://a." })"sv, "https://a.example/x"sv));
    EXPECT(matches(R"({ "urlSuffix": "/x" })"sv, "https://a.example/x"sv));
    // urlMatches tests the URL without its fragment, and originAndPathMatches without its query too.
    EXPECT(matches(R"({ "urlMatches": "x\\?q=1$" })"sv, "https://a.example/x?q=1#frag"sv));
    EXPECT(!matches(R"({ "urlMatches": "frag" })"sv, "https://a.example/x?q=1#frag"sv));
    EXPECT(matches(R"({ "originAndPathMatches": "^https://a\\.example/x$" })"sv, "https://a.example/x?q=1"sv));
    EXPECT(!matches(R"({ "originAndPathMatches": "q=1" })"sv, "https://a.example/x?q=1"sv));
}

TEST_CASE(scheme_and_port_keys)
{
    EXPECT(matches(R"({ "schemes": ["https", "wss"] })"sv, "https://a.example/"sv));
    EXPECT(!matches(R"({ "schemes": ["http"] })"sv, "https://a.example/"sv));
    // A URL without a port has its scheme's default.
    EXPECT(matches(R"({ "ports": [443] })"sv, "https://a.example/"sv));
    EXPECT(matches(R"({ "ports": [80, [8000, 8999]] })"sv, "http://a.example:8080/"sv));
    EXPECT(!matches(R"({ "ports": [[8000, 8079]] })"sv, "http://a.example:8080/"sv));
}

TEST_CASE(url_without_a_host)
{
    // A URL with no host, like about:blank's, has an empty host, which only equality can match.
    EXPECT(!matches(R"({ "hostContains": "a" })"sv, "about:blank"sv));
    EXPECT(matches(R"({ "hostEquals": "" })"sv, "about:blank"sv));
    EXPECT(matches(R"({ "urlEquals": "about:blank" })"sv, "about:blank"sv));
    EXPECT(matches(R"({ "schemes": ["about"] })"sv, "about:blank"sv));
}

TEST_CASE(every_given_key_must_match)
{
    EXPECT(matches(R"({ "hostSuffix": "example.com", "pathPrefix": "/a" })"sv, "https://www.example.com/a/b"sv));
    EXPECT(!matches(R"({ "hostSuffix": "example.com", "pathPrefix": "/b" })"sv, "https://www.example.com/a/b"sv));
    EXPECT(matches(R"({})"sv, "https://anything.example/"sv));
}

TEST_CASE(invalid_filters_are_errors)
{
    EXPECT_EQ(parse_error(R"({ "hostname": "a.example" })"sv), R"(Unexpected property "hostname")"sv);
    EXPECT(parse_error(R"({ "hostEquals": 1 })"sv).contains("hostEquals"sv));
    EXPECT(parse_error(R"({ "ports": ["80"] })"sv).contains("ports"sv));
    EXPECT(parse_error(R"({ "urlMatches": "(" })"sv).contains("urlMatches"sv));
}

TEST_CASE(firefox_edge_cases)
{
    // An IPv6 host is compared without its brackets.
    EXPECT(matches(R"({ "hostEquals": "::1" })"sv, "http://[::1]/"sv));
    // A scheme without a default port has no port at all, so no port matches it.
    EXPECT(!matches(R"({ "ports": [0] })"sv, "about:blank"sv));
    EXPECT(matches(R"({ "ports": [80] })"sv, "http://a.example/"sv));
    // An empty Contains, Prefix, or Suffix condition is no condition.
    EXPECT(matches(R"({ "hostContains": "" })"sv, "about:blank"sv));
    EXPECT(matches(R"({ "pathPrefix": "", "querySuffix": "" })"sv, "https://a.example/"sv));
}
