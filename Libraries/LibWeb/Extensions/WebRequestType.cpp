/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Extensions/WebRequestType.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

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

}
