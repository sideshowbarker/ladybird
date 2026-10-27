/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/WebRequestType.h>
#include <LibWeb/Fetch/Fetching/ClientContextSnapshots.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>

namespace Web::Extensions {

StringView web_request_type_for(Fetch::Infrastructure::Request const& request)
{
    using Request = Fetch::Infrastructure::Request;

    // A request with no destination is told apart by the API that made it.
    if (!request.destination().has_value()) {
        switch (request.initiator_type().value_or(Request::InitiatorType::Other)) {
        case Request::InitiatorType::Fetch:
        case Request::InitiatorType::XMLHttpRequest:
            return "xmlhttprequest"sv;
        case Request::InitiatorType::Beacon:
            return "beacon"sv;
        case Request::InitiatorType::Ping:
            return "ping"sv;
        default:
            return "other"sv;
        }
    }

    switch (*request.destination()) {
    case Request::Destination::Document:
        return "main_frame"sv;
    case Request::Destination::IFrame:
    case Request::Destination::Frame:
        return "sub_frame"sv;
    case Request::Destination::Object:
    case Request::Destination::Embed:
        return "object"sv;
    case Request::Destination::Style:
        return "stylesheet"sv;
    case Request::Destination::Script:
    case Request::Destination::Worker:
    case Request::Destination::SharedWorker:
    case Request::Destination::ServiceWorker:
    case Request::Destination::AudioWorklet:
    case Request::Destination::PaintWorklet:
        return "script"sv;
    case Request::Destination::Image:
        return request.initiator() == Request::Initiator::ImageSet ? "imageset"sv : "image"sv;
    case Request::Destination::Font:
        return "font"sv;
    case Request::Destination::Audio:
    case Request::Destination::Video:
    case Request::Destination::Track:
        return "media"sv;
    case Request::Destination::Report:
        return "csp_report"sv;
    case Request::Destination::XSLT:
        return "xslt"sv;
    case Request::Destination::JSON:
        return "json"sv;
    case Request::Destination::Manifest:
        return "web_manifest"sv;
    case Request::Destination::Text:
    case Request::Destination::WebIdentity:
        return "other"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Requests::WebRequestContext> web_request_context_for(Fetch::Infrastructure::Request& request, Page& page)
{
    auto extensions = ExtensionSet::the().extensions();
    if (!any_of(extensions, [](auto const& extension) { return extension.has_permission("webRequest"sv); }))
        return {};

    auto to_navigable = [](HTML::CrossProcessId id) { return Requests::WebRequestNavigable { id.namespace_id, id.local_id }; };

    Requests::WebRequestContext context;
    context.serial = request.web_request_serial();
    context.type = MUST(String::from_utf8(web_request_type_for(request)));
    context.page_id = page.has_top_level_traversable() ? page.client().id().value() : 0;
    context.is_navigation = request.mode() == Fetch::Infrastructure::Request::Mode::Navigate;

    auto client = Fetch::Fetching::resolve_client(request);
    auto document = client ? client->responsible_document() : nullptr;
    // An about:blank or srcdoc document's requests are made on behalf of the document that created it, whose URL its
    // fallback base URL is; so that's the document extensions see, and need host access to.
    Optional<URL::URL> document_url;
    if (document) {
        document_url = document->url();
        if (document_url->scheme() == "about"sv && document_url->serialize_path().is_one_of("blank"sv, "srcdoc"sv))
            document_url = document->fallback_base_url();
        context.origin_url = document_url;
    }

    if (context.is_navigation) {
        if (auto const& navigable = request.web_request_navigable(); navigable.has_value())
            context.navigable = to_navigable(*navigable);
    } else if (document) {
        context.document_url = document_url;
        if (auto navigable = document->navigable())
            context.navigable = to_navigable(navigable->id());
    }
    return context;
}

}
