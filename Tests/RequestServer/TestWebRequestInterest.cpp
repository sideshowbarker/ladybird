/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/WebRequestInterest.h>

using Requests::WebRequestEvent;

static URL::URL url(StringView string)
{
    return URL::Parser::basic_parse(string).release_value();
}

TEST_CASE(interest_skips_unmatched_requests)
{
    RequestServer::WebRequestInterestSet interest;
    EXPECT(!interest.should_report(WebRequestEvent::BeforeRequest, url("https://a.example/"sv), "script"sv));

    interest.set({
        { .event = WebRequestEvent::BeforeRequest, .urls = { "*://a.example/*"_string }, .types = {}, .blocking = true },
        { .event = WebRequestEvent::Completed, .urls = { "<all_urls>"_string }, .types = { "script"_string }, .blocking = false },
        { .event = WebRequestEvent::Completed, .urls = { "not a pattern"_string }, .types = {}, .blocking = false },
    });
    EXPECT(interest.should_report(WebRequestEvent::BeforeRequest, url("https://a.example/x.js"sv), "script"sv));
    EXPECT(!interest.should_report(WebRequestEvent::BeforeRequest, url("https://b.example/x.js"sv), "script"sv));
    EXPECT(!interest.should_report(WebRequestEvent::SendHeaders, url("https://a.example/x.js"sv), "script"sv));
    EXPECT(interest.should_report(WebRequestEvent::Completed, url("https://b.example/x.js"sv), "script"sv));
    EXPECT(!interest.should_report(WebRequestEvent::Completed, url("https://b.example/x.png"sv), "image"sv));
    EXPECT(interest.wants_answer(WebRequestEvent::BeforeRequest, url("https://a.example/x.js"sv), "script"sv));
    EXPECT(!interest.wants_answer(WebRequestEvent::Completed, url("https://b.example/x.js"sv), "script"sv));

    interest.set({});
    EXPECT(!interest.should_report(WebRequestEvent::Completed, url("https://b.example/x.js"sv), "script"sv));
}
