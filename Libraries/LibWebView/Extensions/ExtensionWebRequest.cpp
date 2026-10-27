/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
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
    if (m_contexts.remove({ &endpoint, context_id }))
        notify_interest_changed();
}

void ExtensionWebRequestRouter::endpoint_destroyed(Endpoint& endpoint)
{
    if (m_contexts.remove_all_matching([&](auto const& key, auto const&) { return key.endpoint == &endpoint; }))
        notify_interest_changed();
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

String ExtensionWebRequestRouter::request_id_for(int client_id, u64 serial)
{
    return m_request_ids.ensure({ client_id, serial }, [&] { return String::number(++m_next_request_id); });
}

void ExtensionWebRequestRouter::forget_request(int client_id, u64 serial)
{
    m_request_ids.remove({ client_id, serial });
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
    return any_of(listener.urls, [&](auto const& pattern) { return pattern.matches(details.url); });
}

void ExtensionWebRequestRouter::report(int client_id, Requests::WebRequestEventDetails const& details, WebRequestPlace const& place)
{
    auto const& context = details.context;
    auto request_id = request_id_for(client_id, context.serial);
    if (details.event == Requests::WebRequestEvent::Completed || details.event == Requests::WebRequestEvent::ErrorOccurred)
        forget_request(client_id, context.serial);

    // A frame's navigation is made in its parent's document; the top-level one is made in none.
    auto document_url = context.is_navigation ? place.parent_document_url : context.document_url;
    if (context.is_navigation && context.type == "main_frame"sv)
        document_url.clear();

    struct Delivery {
        ContextKey key;
        Vector<u64> listener_ids;
        bool request_headers { false };
        bool response_headers { false };
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
        }
        if (!delivery.listener_ids.is_empty())
            deliveries.append(move(delivery));
    }
    if (deliveries.is_empty())
        return;

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
    if (context.origin_url.has_value())
        base.set("originUrl"sv, context.origin_url->serialize());
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
        delivery.key.endpoint->dispatch_extension_web_request_event(delivery.key.context_id, ++m_next_dispatch_id, move(delivery.listener_ids), object.serialized(), false);
    }
}

}
