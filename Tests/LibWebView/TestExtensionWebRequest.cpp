/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
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
        u64 dispatch_id;
        Vector<u64> listener_ids;
        String details_json;
        bool wants_answer;
    };
    virtual void dispatch_extension_web_request_event(u64 context_id, u64 dispatch_id, Vector<u64> listener_ids, String details_json, bool wants_answer) override
    {
        dispatches.append({ context_id, dispatch_id, move(listener_ids), move(details_json), wants_answer });
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
    Web::Extensions::ExtensionDescriptor blocker;
    blocker.id = "blocker"_string;
    blocker.api_permissions = { "webRequest"_string, "webRequestBlocking"_string };
    router.descriptor_for = [&](StringView id) -> Web::Extensions::ExtensionDescriptor const* { return id == "blocker"sv ? &blocker : nullptr; };
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
}

static Web::Extensions::ExtensionDescriptor descriptor(StringView id)
{
    Web::Extensions::ExtensionDescriptor descriptor;
    descriptor.id = MUST(String::from_utf8(id));
    descriptor.host_permissions = { MUST(MatchPattern::parse("<all_urls>"sv)) };
    descriptor.api_permissions = { "webRequest"_string, "webRequestBlocking"_string };
    return descriptor;
}

static Requests::WebRequestEventDetails before_request(StringView url)
{
    Requests::WebRequestEventDetails details;
    details.event = WebRequestEvent::BeforeRequest;
    details.url = URL::Parser::basic_parse(url).release_value();
    details.method = "GET";
    details.context.serial = 1;
    details.context.type = "script"_string;
    return details;
}

struct Decisions {
    Vector<Requests::WebRequestDecision> decisions;
};

static void set_up(WebView::ExtensionWebRequestRouter& router, Vector<Web::Extensions::ExtensionDescriptor> const& descriptors, Decisions& decisions)
{
    router.descriptor_for = [&descriptors](StringView id) -> Web::Extensions::ExtensionDescriptor const* {
        for (auto const& descriptor : descriptors) {
            if (descriptor.id == id)
                return &descriptor;
        }
        return nullptr;
    };
    // Each extension is installed after the ones listed before it.
    router.install_rank = [&descriptors](StringView id) -> size_t {
        for (size_t i = 0; i < descriptors.size(); ++i) {
            if (descriptors[i].id == id)
                return i;
        }
        return 0;
    };
    router.on_decision = [&decisions](int, u64, bool, Requests::WebRequestDecision decision) { decisions.decisions.append(move(decision)); };
}

static JsonValue json(StringView text)
{
    return JsonValue::from_string(text).release_value();
}

TEST_CASE(cancel_wins_over_redirect)
{
    WebView::ExtensionWebRequestRouter router;
    auto decision = router.merge_answers(WebRequestEvent::BeforeRequest,
        { { "later"_string, json(R"({ "redirectUrl": "https://b.example/" })"sv) }, { "earlier"_string, json(R"({ "cancel": true })"sv) } });
    EXPECT(decision.cancel);
    EXPECT(!decision.redirect_url.has_value());
}

TEST_CASE(later_installed_extension_headers_win)
{
    WebView::ExtensionWebRequestRouter router;
    Vector descriptors { descriptor("first"sv), descriptor("second"sv) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    auto decision = router.merge_answers(WebRequestEvent::BeforeSendHeaders,
        { { "second"_string, json(R"({ "requestHeaders": [{ "name": "Referer", "value": "https://www.google.com/" }] })"sv) },
            { "first"_string, json(R"({ "requestHeaders": [{ "name": "Referer", "value": "https://first.example/" }] })"sv) } });
    EXPECT(!decision.cancel);
    VERIFY(decision.request_headers.has_value());
    EXPECT_EQ(decision.request_headers->size(), 1u);
    EXPECT_EQ(decision.request_headers->first().name, "Referer"sv);
    EXPECT_EQ(decision.request_headers->first().value, "https://www.google.com/"sv);

    // An event's answer can change only what that event may change.
    auto ignored = router.merge_answers(WebRequestEvent::BeforeRequest, { { "second"_string, json(R"({ "requestHeaders": [] })"sv) } });
    EXPECT(!ignored.request_headers.has_value());
}

TEST_CASE(pending_blocking_dispatches_answer_empty_when_context_goes)
{
    WebView::ExtensionWebRequestRouter router;
    Vector descriptors { descriptor("blocker"sv) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    TestEndpoint endpoint;
    router.listeners_changed(endpoint, "blocker"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });

    router.report(1, 7, before_request("https://a.example/x.js"sv), {}, true);
    VERIFY(endpoint.dispatches.size() == 1);
    EXPECT(endpoint.dispatches[0].wants_answer);
    EXPECT(decisions.decisions.is_empty());

    router.context_destroyed(endpoint, 1);
    VERIFY(decisions.decisions.size() == 1);
    EXPECT(!decisions.decisions[0].cancel);
    EXPECT(!decisions.decisions[0].redirect_url.has_value());
}

TEST_CASE(an_unanswered_listener_counts_as_no_answer)
{
    WebView::ExtensionWebRequestRouter router;
    Vector descriptors { descriptor("blocker"sv) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    TestEndpoint endpoint;
    router.listeners_changed(endpoint, "blocker"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });

    // A listener that threw or whose promise rejected sends nothing back for its answer.
    router.report(1, 7, before_request("https://a.example/x.js"sv), {}, true);
    router.answers_received(endpoint, endpoint.dispatches[0].dispatch_id, {});
    VERIFY(decisions.decisions.size() == 1);
    EXPECT(!decisions.decisions[0].cancel);

    // An answer that cancels cancels the request.
    router.report(1, 8, before_request("https://a.example/y.js"sv), {}, true);
    router.answers_received(endpoint, endpoint.dispatches[1].dispatch_id, { R"({ "cancel": true })"_string });
    VERIFY(decisions.decisions.size() == 2);
    EXPECT(decisions.decisions[1].cancel);
}

TEST_CASE(blocking_needs_webRequestBlocking_in_the_ui_process)
{
    WebView::ExtensionWebRequestRouter router;
    auto observer = descriptor("observer"sv);
    observer.api_permissions = { "webRequest"_string };
    Vector descriptors { move(observer) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    TestEndpoint endpoint;
    // A renderer claiming a blocking listener for an extension without webRequestBlocking gets an observing one.
    router.listeners_changed(endpoint, "observer"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });
    EXPECT(!router.interest().first().blocking);
    router.report(1, 7, before_request("https://a.example/x.js"sv), {}, true);
    EXPECT_EQ(decisions.decisions.size(), 1u);
    VERIFY(endpoint.dispatches.size() == 1);
    EXPECT(!endpoint.dispatches[0].wants_answer);
}

TEST_CASE(incognito_filter_matches_the_request_privacy)
{
    WebView::ExtensionWebRequestRouter router;
    Vector descriptors { descriptor("watcher"sv) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    router.allowed_in_private = [](StringView) { return true; };
    TestEndpoint endpoint;
    auto ordinary_only = listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv);
    ordinary_only.incognito = false;
    auto private_only = listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv);
    private_only.incognito = true;
    router.listeners_changed(endpoint, "watcher"_string, 1, { ordinary_only, private_only });

    WebView::WebRequestPlace place;
    place.is_private = true;
    router.report(1, 7, before_request("https://a.example/x.js"sv), place, false);
    VERIFY(endpoint.dispatches.size() == 1);
    EXPECT_EQ(endpoint.dispatches[0].listener_ids, (Vector { private_only.id }));
    place.is_private = false;
    router.report(1, 8, before_request("https://a.example/y.js"sv), place, false);
    VERIFY(endpoint.dispatches.size() == 2);
    EXPECT_EQ(endpoint.dispatches[1].listener_ids, (Vector { ordinary_only.id }));
}

TEST_CASE(an_unverified_request_is_reported_to_no_one)
{
    WebView::ExtensionWebRequestRouter router;
    Vector descriptors { descriptor("blocker"sv) };
    Decisions decisions;
    set_up(router, descriptors, decisions);
    TestEndpoint endpoint;
    router.listeners_changed(endpoint, "blocker"_string, 1, { listener(WebRequestEvent::BeforeRequest, "<all_urls>"sv, true) });

    // A request whose page or frame the UI process can't find came from a renderer it can't take at its word.
    WebView::WebRequestPlace place;
    place.is_verified = false;
    router.report(1, 7, before_request("https://a.example/x.js"sv), place, true);
    EXPECT(endpoint.dispatches.is_empty());
    VERIFY(decisions.decisions.size() == 1);
    EXPECT(!decisions.decisions[0].cancel);
}

TEST_CASE(startup_gate_on_with_a_blocking_extension)
{
    WebView::ExtensionWebRequestStartupGate gate;
    Vector<bool> changes;
    gate.on_change = [&](bool on) { changes.append(on); };
    gate.start({ "blocker"_string });
    EXPECT(gate.is_on());
    EXPECT_EQ(changes, (Vector { true }));
    EXPECT_EQ(gate.timeout(), AK::Duration::from_seconds(10));
}

TEST_CASE(startup_gate_off_when_every_blocking_background_finishes)
{
    WebView::ExtensionWebRequestStartupGate gate;
    Vector<bool> changes;
    gate.on_change = [&](bool on) { changes.append(on); };
    gate.start({ "first"_string, "second"_string });
    gate.background_settled("first"sv);
    EXPECT(gate.is_on());
    // An extension that holds nothing back changes nothing.
    gate.background_settled("other"sv);
    EXPECT(gate.is_on());
    gate.background_settled("second"sv);
    EXPECT(!gate.is_on());
    EXPECT_EQ(changes, (Vector { true, false }));
}

TEST_CASE(startup_gate_off_after_ten_seconds)
{
    Core::EventLoop loop;
    WebView::ExtensionWebRequestStartupGate gate { AK::Duration::from_milliseconds(50) };
    Vector<bool> changes;
    gate.on_change = [&](bool on) {
        changes.append(on);
        if (!on)
            loop.quit(0);
    };
    gate.start({ "never-loads"_string });
    auto deadline = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] { loop.quit(1); });
    deadline->start();
    EXPECT_EQ(loop.exec(), 0);
    EXPECT(!gate.is_on());
    EXPECT_EQ(changes, (Vector { true, false }));
}

TEST_CASE(startup_gate_off_without_webRequestBlocking)
{
    WebView::ExtensionWebRequestStartupGate gate;
    Vector<bool> changes;
    gate.on_change = [&](bool on) { changes.append(on); };
    gate.start({});
    EXPECT(!gate.is_on());
    EXPECT(changes.is_empty());
}
