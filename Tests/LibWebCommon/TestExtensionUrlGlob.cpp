/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/UrlGlob.h>

using Web::Extensions::url_glob_matches;
using Web::Extensions::url_matches_globs;

static bool globs_match(Vector<String> include_globs, Vector<String> exclude_globs, StringView url)
{
    return url_matches_globs(include_globs, exclude_globs, URL::Parser::basic_parse(url).release_value());
}

TEST_CASE(star_matches_any_run)
{
    EXPECT(url_glob_matches("https://*.example.com/*"sv, "https://www.example.com/page"sv));
    EXPECT(url_glob_matches("https://*example.com/*"sv, "https://example.com/"sv));
    EXPECT(url_glob_matches("*"sv, ""sv));
    EXPECT(url_glob_matches("**a**"sv, "a"sv));
    EXPECT(!url_glob_matches("https://*.example.com/*"sv, "https://example.org/"sv));
}

TEST_CASE(question_mark_matches_one_character)
{
    EXPECT(url_glob_matches("https://a?c.com/"sv, "https://abc.com/"sv));
    EXPECT(!url_glob_matches("https://a?c.com/"sv, "https://ac.com/"sv));
    EXPECT(!url_glob_matches("https://a?c.com/"sv, "https://abbc.com/"sv));
}

TEST_CASE(the_glob_covers_the_whole_url)
{
    EXPECT(!url_glob_matches("example.com"sv, "https://example.com/"sv));
    EXPECT(!url_glob_matches("https://example.com/"sv, "https://example.com/page"sv));
    EXPECT(url_glob_matches("*example.com*"sv, "https://example.com/"sv));
}

TEST_CASE(include_globs_need_one_match)
{
    EXPECT(globs_match({ "*/a/*"_string, "*/b/*"_string }, {}, "https://example.com/b/page"sv));
    EXPECT(!globs_match({ "*/a/*"_string, "*/b/*"_string }, {}, "https://example.com/c/page"sv));
}

TEST_CASE(exclude_globs_reject)
{
    EXPECT(!globs_match({}, { "*/private/*"_string }, "https://example.com/private/page"sv));
    EXPECT(!globs_match({ "*example.com*"_string }, { "*/private/*"_string }, "https://example.com/private/page"sv));
    EXPECT(globs_match({}, { "*/private/*"_string }, "https://example.com/public/page"sv));
}

TEST_CASE(no_globs_match_everything)
{
    EXPECT(globs_match({}, {}, "https://example.com/"sv));
    EXPECT(globs_match({}, {}, "data:text/html,hi"sv));
}
