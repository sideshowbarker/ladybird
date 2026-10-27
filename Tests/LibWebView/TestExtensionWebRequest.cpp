/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionWebRequest.h>

using Requests::WebRequestEvent;
using Requests::WebRequestInterest;
using Web::Extensions::MatchPattern;
using Web::Extensions::WebRequestListener;

namespace {

class TestEndpoint final : public WebView::ExtensionWebRequestRouter::Endpoint {
public:
    struct Dispatch {
        u64 context_id;
        Vector<u64> listener_ids;
        String details_json;
        bool wants_answer;
    };
    virtual void dispatch_extension_web_request_event(u64 context_id, u64, Vector<u64> listener_ids, String details_json, bool wants_answer) override
    {
        dispatches.append({ context_id, move(listener_ids), move(details_json), wants_answer });
    }
    Vector<Dispatch> dispatches;
};

}

static WebRequestListener listener(WebRequestEvent event, StringView pattern, bool blocking = false, Vector<String> types = {})
{
    static u64 next_id { 0 };
    WebRequestListener listener;
    listener.id = ++next_id;
    listener.event = event;
    listener.urls = { MUST(MatchPattern::parse(pattern)) };
    listener.types = move(types);
    listener.blocking = blocking;
    return listener;
}

static WebRequestInterest interest(WebRequestEvent event, StringView pattern, bool blocking = false, Vector<String> types = {})
{
    return WebRequestInterest { .event = event, .urls = { MUST(String::from_utf8(pattern)) }, .types = move(types), .blocking = blocking };
}

TEST_CASE(interest_folds_listeners)
{
    WebView::ExtensionWebRequestRouter router;
    TestEndpoint first;
    TestEndpoint second;
    router.listeners_changed(first, "blocker"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true), listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });
    router.listeners_changed(second, "watcher"_string, 1, { listener(WebRequestEvent::BeforeRequest, "*://a.example/*"sv), listener(WebRequestEvent::Completed, "<all_urls>"sv, false, { "script"_string }) });

    auto folded = router.interest();
    EXPECT_EQ(folded.size(), 3u);
    EXPECT(folded.contains_slow(interest(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true)));
    EXPECT(folded.contains_slow(interest(WebRequestEvent::BeforeRequest, "*://a.example/*"sv)));
    EXPECT(folded.contains_slow(interest(WebRequestEvent::Completed, "<all_urls>"sv, false, { "script"_string })));
}

TEST_CASE(interest_drops_a_destroyed_context)
{
    WebView::ExtensionWebRequestRouter router;
    TestEndpoint endpoint;
    TestEndpoint other;
    size_t changes = 0;
    router.on_interest_changed = [&] { ++changes; };

    router.listeners_changed(endpoint, "blocker"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });
    // The same context id in another process is another context.
    router.listeners_changed(other, "blocker"_string, 1, { listener(WebRequestEvent::Completed, "<all_urls>"sv) });
    EXPECT_EQ(router.interest().size(), 2u);
    EXPECT_EQ(changes, 2u);

    router.context_destroyed(endpoint, 1);
    EXPECT_EQ(router.interest(), (Vector { interest(WebRequestEvent::Completed, "<all_urls>"sv) }));
    EXPECT_EQ(changes, 3u);

    // A context nothing was heard from changes nothing.
    router.context_destroyed(endpoint, 7);
    EXPECT_EQ(changes, 3u);

    router.endpoint_destroyed(other);
    EXPECT(router.interest().is_empty());
    EXPECT_EQ(changes, 4u);
}

TEST_CASE(request_id_is_stable_per_serial_and_unique_per_client)
{
    WebView::ExtensionWebRequestRouter router;
    auto first = router.request_id_for(1, 10);
    EXPECT_EQ(router.request_id_for(1, 10), first);
    auto other_client = router.request_id_for(2, 10);
    EXPECT_NE(other_client, first);
    auto other_serial = router.request_id_for(1, 11);
    EXPECT_NE(other_serial, first);
    EXPECT_NE(other_serial, other_client);
    EXPECT(first.to_number<u64>().has_value());

    router.forget_request(1, 10);
    EXPECT_NE(router.request_id_for(1, 10), first);
}
