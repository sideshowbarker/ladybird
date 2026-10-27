/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Extensions/WebRequestType.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

using Request = Web::Fetch::Infrastructure::Request;

static StringView type_of(Optional<Request::Destination> destination, Optional<Request::InitiatorType> initiator_type = {}, Optional<Request::Initiator> initiator = {}, bool navigation = false)
{
    auto request = Request::create();
    request->set_destination(destination);
    request->set_initiator_type(initiator_type);
    request->set_initiator(initiator);
    if (navigation)
        request->set_mode(Request::Mode::Navigate);
    return Web::Extensions::web_request_type_for(request);
}

TEST_CASE(web_request_type_for_maps_destinations)
{
    auto vm = JS::VM::create();

    EXPECT_EQ(type_of(Request::Destination::Document, {}, {}, true), "main_frame"sv);
    EXPECT_EQ(type_of(Request::Destination::IFrame, {}, {}, true), "sub_frame"sv);
    EXPECT_EQ(type_of(Request::Destination::Frame, {}, {}, true), "sub_frame"sv);
    EXPECT_EQ(type_of(Request::Destination::Object, {}, {}, true), "object"sv);
    EXPECT_EQ(type_of(Request::Destination::Embed), "object"sv);
    EXPECT_EQ(type_of(Request::Destination::Style), "stylesheet"sv);
    EXPECT_EQ(type_of(Request::Destination::Script), "script"sv);
    EXPECT_EQ(type_of(Request::Destination::Worker), "script"sv);
    EXPECT_EQ(type_of(Request::Destination::SharedWorker), "script"sv);
    EXPECT_EQ(type_of(Request::Destination::AudioWorklet), "script"sv);
    EXPECT_EQ(type_of(Request::Destination::Image), "image"sv);
    EXPECT_EQ(type_of(Request::Destination::Image, {}, Request::Initiator::ImageSet), "imageset"sv);
    EXPECT_EQ(type_of(Request::Destination::Font), "font"sv);
    EXPECT_EQ(type_of(Request::Destination::Audio), "media"sv);
    EXPECT_EQ(type_of(Request::Destination::Video), "media"sv);
    EXPECT_EQ(type_of(Request::Destination::Track), "media"sv);
    EXPECT_EQ(type_of(Request::Destination::Report), "csp_report"sv);
    EXPECT_EQ(type_of(Request::Destination::XSLT), "xslt"sv);
    EXPECT_EQ(type_of(Request::Destination::JSON), "json"sv);
    EXPECT_EQ(type_of(Request::Destination::Manifest), "web_manifest"sv);
    EXPECT_EQ(type_of({}, Request::InitiatorType::Fetch), "xmlhttprequest"sv);
    EXPECT_EQ(type_of({}, Request::InitiatorType::XMLHttpRequest), "xmlhttprequest"sv);
    EXPECT_EQ(type_of({}, Request::InitiatorType::Beacon), "beacon"sv);
    EXPECT_EQ(type_of({}, Request::InitiatorType::Ping), "ping"sv);
    EXPECT_EQ(type_of({}), "other"sv);
    EXPECT_EQ(type_of(Request::Destination::Text), "other"sv);
}
