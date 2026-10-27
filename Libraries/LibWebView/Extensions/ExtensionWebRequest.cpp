/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Extensions/ExtensionWebRequest.h>

namespace WebView {

void ExtensionWebRequestRouter::notify_interest_changed()
{
    if (on_interest_changed)
        on_interest_changed();
}

void ExtensionWebRequestRouter::listeners_changed(Endpoint& endpoint, String const& extension_id, u64 context_id, Vector<Web::Extensions::WebRequestListener> listeners)
{
    ContextKey key { &endpoint, context_id };
    // The renderer's word that a listener blocks counts only for an extension that may block.
    auto const* descriptor = descriptor_for ? descriptor_for(extension_id) : nullptr;
    if (!descriptor || !descriptor->has_permission("webRequestBlocking"sv)) {
        for (auto& listener : listeners)
            listener.blocking = false;
    }
    if (listeners.is_empty()) {
        if (m_contexts.remove(key))
            notify_interest_changed();
        return;
    }
    m_contexts.set(key, { extension_id, move(listeners) });
    notify_interest_changed();
}

void ExtensionWebRequestRouter::context_destroyed(Endpoint& endpoint, u64 context_id)
{
    ContextKey key { &endpoint, context_id };
    settle_dispatches_of([&](ContextKey const& context) { return context == key; });
    if (m_contexts.remove(key))
        notify_interest_changed();
}

void ExtensionWebRequestRouter::endpoint_destroyed(Endpoint& endpoint)
{
    settle_dispatches_of([&](ContextKey const& context) { return context.endpoint == &endpoint; });
    if (m_contexts.remove_all_matching([&](auto const& key, auto const&) { return key.endpoint == &endpoint; }))
        notify_interest_changed();
}

// A context that's gone answers its outstanding dispatches with nothing, so no request waits on it.
void ExtensionWebRequestRouter::settle_dispatches_of(Function<bool(ContextKey const&)> const& is_gone)
{
    Vector<u64> gone;
    for (auto const& [dispatch_id, dispatch] : m_pending_dispatches) {
        if (is_gone(dispatch.context))
            gone.append(dispatch_id);
    }
    for (auto dispatch_id : gone)
        settle_dispatch(dispatch_id, {});
}

void ExtensionWebRequestRouter::answers_received(Endpoint& endpoint, u64 dispatch_id, Vector<String> answers_json)
{
    auto dispatch = m_pending_dispatches.get(dispatch_id);
    if (!dispatch.has_value() || dispatch->context.endpoint != &endpoint)
        return;
    settle_dispatch(dispatch_id, move(answers_json));
}

void ExtensionWebRequestRouter::settle_dispatch(u64 dispatch_id, Vector<String> answers_json)
{
    auto dispatch = m_pending_dispatches.take(dispatch_id);
    if (!dispatch.has_value())
        return;
    auto decision = m_pending_decisions.get(dispatch->decision_id);
    if (!decision.has_value())
        return;
    for (auto const& json : answers_json) {
        if (auto answer = JsonValue::from_string(json); !answer.is_error() && answer.value().is_object())
            decision->answers.append({ dispatch->extension_id, answer.release_value() });
    }
    if (--decision->outstanding > 0)
        return;
    auto pending = m_pending_decisions.take(dispatch->decision_id).release_value();
    if (on_decision)
        on_decision(pending.client_id, pending.request_id, merge_answers(pending.event, pending.answers, pending.initiator));
}

static Optional<Vector<HTTP::Header>> headers_from_json(JsonValue const& value)
{
    if (!value.is_array())
        return {};
    Vector<HTTP::Header> headers;
    for (auto const& entry : value.as_array().values()) {
        if (!entry.is_object())
            return {};
        auto const& object = entry.as_object();
        auto name = object.get_string("name"sv);
        if (!name.has_value())
            return {};
        if (auto text = object.get_string("value"sv); text.has_value()) {
            headers.append({ name->to_byte_string(), text->to_byte_string() });
            continue;
        }
        auto bytes = object.get_array("binaryValue"sv);
        if (!bytes.has_value())
            return {};
        StringBuilder value_bytes;
        for (auto const& byte : bytes->values()) {
            auto number = byte.get_u32();
            if (!number.has_value() || *number > 255)
                return {};
            value_bytes.append(static_cast<char>(*number));
        }
        headers.append({ name->to_byte_string(), value_bytes.to_byte_string() });
    }
    return headers;
}

// Where an extension may send a request: anywhere over HTTP(S), to a data: URL, or to one of its own web-accessible
// resources. Anything else, a javascript: URL among them, counts as no answer.
Optional<URL::URL> ExtensionWebRequestRouter::valid_redirect_target(String const& extension_id, StringView url_string, Optional<URL::URL> const& initiator) const
{
    auto url = URL::Parser::basic_parse(url_string);
    if (!url.has_value())
        return {};
    if (first_is_one_of(url->scheme(), "http"sv, "https"sv, "data"sv))
        return url;
    if (url->scheme() != Web::Extensions::scheme || url->serialized_host() != extension_id)
        return {};
    auto const* descriptor = descriptor_for ? descriptor_for(extension_id) : nullptr;
    if (!descriptor || !initiator.has_value() || !descriptor->is_web_accessible(url->serialize_path(), *initiator))
        return {};
    return url;
}

Requests::WebRequestDecision ExtensionWebRequestRouter::merge_answers(Requests::WebRequestEvent event, Vector<ExtensionAnswer> const& answers, Optional<URL::URL> const& initiator) const
{
    using Requests::WebRequestEvent;
    bool may_cancel = first_is_one_of(event, WebRequestEvent::BeforeRequest, WebRequestEvent::BeforeSendHeaders, WebRequestEvent::HeadersReceived, WebRequestEvent::AuthRequired);
    bool may_redirect = event == WebRequestEvent::BeforeRequest;

    Requests::WebRequestDecision decision;
    if (may_cancel && any_of(answers, [](auto const& answer) { return answer.answer.as_object().get_bool("cancel"sv).value_or(false); })) {
        decision.cancel = true;
        return decision;
    }

    // The most recently installed extension's answer comes first.
    Vector<ExtensionAnswer const*> ordered;
    for (auto const& answer : answers)
        ordered.append(&answer);
    auto rank = [this](String const& id) { return install_rank ? install_rank(id) : 0; };
    quick_sort(ordered, [&](auto const* a, auto const* b) { return rank(a->extension_id) > rank(b->extension_id); });

    for (auto const* answer : ordered) {
        auto const& object = answer->answer.as_object();
        if (may_redirect && !decision.redirect_url.has_value()) {
            if (auto url = object.get_string("redirectUrl"sv); url.has_value())
                decision.redirect_url = valid_redirect_target(answer->extension_id, *url, initiator);
        }
        if (event == WebRequestEvent::BeforeSendHeaders && !decision.request_headers.has_value()) {
            if (auto headers = object.get("requestHeaders"sv); headers.has_value())
                decision.request_headers = headers_from_json(*headers);
        }
        if (event == WebRequestEvent::HeadersReceived && !decision.response_headers.has_value()) {
            if (auto headers = object.get("responseHeaders"sv); headers.has_value())
                decision.response_headers = headers_from_json(*headers);
        }
    }
    return decision;
}

Vector<Requests::WebRequestInterest> ExtensionWebRequestRouter::interest() const
{
    Vector<Requests::WebRequestInterest> interest;
    for (auto const& [key, context] : m_contexts) {
        for (auto const& listener : context.listeners) {
            Requests::WebRequestInterest entry { listener.event, {}, listener.types, listener.blocking };
            for (auto const& url : listener.urls)
                entry.urls.append(url.to_string());
            if (!interest.contains_slow(entry))
                interest.append(move(entry));
        }
    }
    return interest;
}

// The client id goes above the serial's 40 bits, so no two clients' requests share an id and nothing need be kept.
String ExtensionWebRequestRouter::request_id_for(int client_id, u64 serial)
{
    return String::number((static_cast<u64>(static_cast<u32>(client_id)) << 40) | (serial & ((1ull << 40) - 1)));
}

// A request made by one of the browser's own pages, or by any extension's, is no extension's to see.
static bool is_restricted_document(URL::URL const& url)
{
    if (url.scheme() == Web::Extensions::scheme)
        return true;
    if (url.scheme() == "about"sv)
        return !url.serialize_path().is_one_of("blank"sv, "srcdoc"sv);
    return false;
}

bool ExtensionWebRequestRouter::may_see(Web::Extensions::ExtensionDescriptor const& descriptor, URL::URL const& url, Optional<URL::URL> const& document_url, bool is_private) const
{
    if (is_private && !(allowed_in_private && allowed_in_private(descriptor.id)))
        return false;
    if (!descriptor.has_host_access(url))
        return false;
    if (!document_url.has_value())
        return true;
    return !is_restricted_document(*document_url) && descriptor.has_host_access(*document_url);
}

static JsonArray headers_to_json(Vector<HTTP::Header> const& headers)
{
    JsonArray array;
    for (auto const& header : headers) {
        JsonObject object;
        object.set("name"sv, MUST(String::from_byte_string(header.name)));
        // A value that isn't UTF-8 goes as its bytes.
        if (Utf8View { header.value.view() }.validate()) {
            object.set("value"sv, MUST(String::from_byte_string(header.value)));
        } else {
            JsonArray bytes;
            for (auto byte : header.value.bytes())
                bytes.must_append(byte);
            object.set("binaryValue"sv, move(bytes));
        }
        array.must_append(move(object));
    }
    return array;
}

static bool listener_hears(Web::Extensions::WebRequestListener const& listener, Requests::WebRequestEventDetails const& details, WebRequestPlace const& place)
{
    if (listener.event != details.event)
        return false;
    if (!listener.types.is_empty() && !listener.types.contains_slow(details.context.type))
        return false;
    if (listener.tab_id.has_value() && *listener.tab_id != place.tab_id)
        return false;
    if (listener.window_id.has_value() && *listener.window_id != place.window_id)
        return false;
    if (listener.incognito.has_value() && *listener.incognito != place.is_private)
        return false;
    return any_of(listener.urls, [&](auto const& pattern) { return pattern.matches(details.url); });
}

void ExtensionWebRequestRouter::report(int client_id, u64 request_id_in_client, Requests::WebRequestEventDetails const& details, WebRequestPlace const& place, bool wants_answer)
{
    // A renderer whose claims about a request don't check out gets no listener to hear it, nor to hold it up.
    if (!place.is_verified) {
        if (wants_answer && on_decision)
            on_decision(client_id, request_id_in_client, {});
        return;
    }

    auto const& context = details.context;
    auto request_id = request_id_for(client_id, context.serial);

    // A frame's navigation is made in its parent's document; the top-level one is made in none.
    auto document_url = context.is_navigation ? place.parent_document_url : context.document_url;
    if (context.is_navigation && context.type == "main_frame"sv)
        document_url.clear();

    struct Delivery {
        ContextKey key;
        Vector<u64> listener_ids;
        bool request_headers { false };
        bool response_headers { false };
        bool blocking { false };
    };
    Vector<Delivery> deliveries;
    for (auto const& [key, listeners] : m_contexts) {
        auto const* descriptor = descriptor_for ? descriptor_for(listeners.extension_id) : nullptr;
        if (!descriptor || !may_see(*descriptor, details.url, document_url, place.is_private))
            continue;
        Delivery delivery { key, {} };
        for (auto const& listener : listeners.listeners) {
            if (!listener_hears(listener, details, place))
                continue;
            delivery.listener_ids.append(listener.id);
            delivery.request_headers |= listener.request_headers;
            delivery.response_headers |= listener.response_headers;
            delivery.blocking |= listener.blocking;
        }
        if (!delivery.listener_ids.is_empty())
            deliveries.append(move(delivery));
    }
    // Only a blocking listener's answer counts, so a request no blocking listener hears goes on at once.
    auto waits = wants_answer && any_of(deliveries, [](auto const& delivery) { return delivery.blocking; });
    if (wants_answer && !waits && on_decision)
        on_decision(client_id, request_id_in_client, {});
    if (deliveries.is_empty())
        return;
    u64 decision_id = 0;
    if (waits) {
        decision_id = ++m_next_decision_id;
        m_pending_decisions.set(decision_id, { client_id, request_id_in_client, details.event, 0, {}, document_url.has_value() ? document_url : context.origin_url });
    }

    JsonObject base;
    base.set("requestId"sv, request_id);
    base.set("url"sv, details.url.serialize());
    base.set("method"sv, MUST(String::from_byte_string(details.method)));
    base.set("type"sv, context.type);
    base.set("timeStamp"sv, static_cast<double>(UnixDateTime::now().milliseconds_since_epoch()));
    base.set("tabId"sv, place.tab_id);
    base.set("frameId"sv, place.frame_id);
    base.set("parentFrameId"sv, place.parent_frame_id);
    base.set("incognito"sv, place.is_private);
    if (document_url.has_value())
        base.set("documentUrl"sv, document_url->serialize());
    if (context.origin_url.has_value()) {
        base.set("originUrl"sv, context.origin_url->serialize());
        base.set("initiator"sv, context.origin_url->origin().serialize());
    }
    if (details.status_code.has_value())
        base.set("statusCode"sv, *details.status_code);
    if (details.status_line.has_value())
        base.set("statusLine"sv, MUST(String::from_byte_string(*details.status_line)));
    if (details.ip.has_value())
        base.set("ip"sv, MUST(String::from_byte_string(*details.ip)));
    if (first_is_one_of(details.event, Requests::WebRequestEvent::HeadersReceived, Requests::WebRequestEvent::ResponseStarted, Requests::WebRequestEvent::BeforeRedirect, Requests::WebRequestEvent::Completed, Requests::WebRequestEvent::ErrorOccurred))
        base.set("fromCache"sv, details.from_cache);
    if (details.redirect_url.has_value())
        base.set("redirectUrl"sv, details.redirect_url->serialize());
    if (details.error.has_value())
        base.set("error"sv, MUST(String::from_byte_string(*details.error)));

    for (auto& delivery : deliveries) {
        auto object = base;
        if (delivery.request_headers && details.request_headers.has_value())
            object.set("requestHeaders"sv, headers_to_json(*details.request_headers));
        if (delivery.response_headers && details.response_headers.has_value())
            object.set("responseHeaders"sv, headers_to_json(*details.response_headers));
        auto dispatch_id = ++m_next_dispatch_id;
        auto delivery_waits = waits && delivery.blocking;
        if (delivery_waits) {
            m_pending_dispatches.set(dispatch_id, { decision_id, delivery.key, m_contexts.get(delivery.key)->extension_id });
            ++m_pending_decisions.get(decision_id)->outstanding;
        }
        delivery.key.endpoint->dispatch_extension_web_request_event(delivery.key.context_id, dispatch_id, move(delivery.listener_ids), object.serialized(), delivery_waits);
    }
}

}
