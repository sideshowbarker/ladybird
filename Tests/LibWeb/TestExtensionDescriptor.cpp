/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/MessageResult.h>
#include <LibWebCommon/Extensions/Storage.h>

using namespace Web::Extensions;

template<typename T>
static T round_trip(T const& value)
{
    IPC::MessageBuffer message_buffer;
    IPC::Encoder encoder { message_buffer };
    MUST(encoder.encode(value));

    auto data = message_buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<T>());
}

TEST_CASE(match_pattern_round_trips_through_ipc)
{
    auto pattern = MUST(MatchPattern::parse("*://*.example.com/a/*"sv));
    EXPECT_EQ(round_trip(pattern), pattern);
    EXPECT_EQ(round_trip(MatchPattern::all_urls()), MatchPattern::all_urls());
}

TEST_CASE(descriptor_round_trips_through_ipc)
{
    ContentScriptDescriptor first_script {
        .id = "registered"_string,
        .matches = { MUST(MatchPattern::parse("https://e.com/*"sv)), MatchPattern::all_urls() },
        .exclude_matches = { MUST(MatchPattern::parse("https://e.com/private/*"sv)) },
        .js_paths = { "a.js"_string, "b.js"_string },
        .js_sources = { "console.log('a')"_string, "console.log('b')"_string },
        .css_paths = { "style.css"_string },
        .css_sources = { "p { color: red }"_string },
        .run_at = RunAt::DocumentStart,
        .all_frames = true,
        .match_about_blank = true,
        .world = ExecutionWorld::Main,
    };
    ContentScriptDescriptor second_script {
        .id = {},
        .matches = { MUST(MatchPattern::parse("file:///*"sv)) },
        .exclude_matches = {},
        .js_paths = { "c.js"_string },
        .js_sources = { "/* c */"_string },
        .css_paths = {},
        .css_sources = {},
        .run_at = RunAt::DocumentIdle,
        .all_frames = false,
        .match_about_blank = false,
        .world = ExecutionWorld::Isolated,
    };
    ExtensionDescriptor descriptor {
        .id = "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string,
        .revision = 42,
        .name = "Test Extension"_string,
        .version = "1.2.3"_string,
        .manifest_version = 2,
        .manifest_json = "{\"name\":\"Test Extension\"}"_string,
        .api_permissions = { "storage"_string, "tabs"_string },
        .host_permissions = { MUST(MatchPattern::parse("https://example.com/*"sv)) },
        .extension_pages_csp = "script-src 'self'; object-src 'self'"_string,
        .web_accessible_resources = { { .resource_globs = { "images/*.png"_string }, .matches = { MatchPattern::all_urls() } } },
        .content_scripts = { first_script, second_script },
        .has_background = true,
        .background_document_html = "<!DOCTYPE html><script src=\"/bg.js\"></script>"_string,
    };

    descriptor.messages.set("hello"_string, "Bonjour $1"_string);
    descriptor.ui_locale = "fr-CA"_string;
    auto decoded = round_trip(descriptor);
    EXPECT_EQ(decoded, descriptor);
    EXPECT_EQ(decoded.content_scripts.size(), 2u);
    EXPECT_EQ(decoded.content_scripts[0].run_at, RunAt::DocumentStart);
    EXPECT_EQ(decoded.content_scripts[0].world, ExecutionWorld::Main);
    EXPECT_EQ(decoded.content_scripts[1].run_at, RunAt::DocumentIdle);
    EXPECT(!decoded.content_scripts[1].all_frames);
    EXPECT_EQ(decoded.web_accessible_resources[0].resource_globs[0], "images/*.png"sv);
}

TEST_CASE(descriptor_defaults)
{
    ContentScriptDescriptor script;
    EXPECT_EQ(script.run_at, RunAt::DocumentIdle);
    EXPECT_EQ(script.world, ExecutionWorld::Isolated);
    EXPECT(!script.all_frames);
    EXPECT(!script.match_about_blank);

    ExtensionDescriptor descriptor;
    EXPECT_EQ(descriptor.manifest_version, 3);
    EXPECT(!descriptor.has_background);
}

TEST_CASE(message_results_round_trip_through_ipc)
{
    for (auto const& result : { MessageResult { MessageResult::Reply { "null"_string } }, MessageResult { MessageResult::NoReply {} }, MessageResult { MessageResult::NoListener {} }, MessageResult { MessageResult::Error { "failed"_string } } })
        EXPECT_EQ(round_trip(result), result);
}

TEST_CASE(storage_messages_round_trip_through_ipc)
{
    for (auto operation : { StorageRequest::Operation::Get, StorageRequest::Operation::Set, StorageRequest::Operation::Remove, StorageRequest::Operation::Clear, StorageRequest::Operation::BytesInUse }) {
        StorageRequest request { operation, Vector<String> { "__proto__"_string, "key"_string }, R"({"key":null})"_string };
        EXPECT_EQ(round_trip(request), request);
        request.keys.clear();
        EXPECT_EQ(round_trip(request), request);
    }
    for (auto type : { StorageResult::Type::Items, StorageResult::Type::Bytes, StorageResult::Type::Empty, StorageResult::Type::Error }) {
        StorageResult result { type, "payload"_string, 10485760 };
        EXPECT_EQ(round_trip(result), result);
    }
}

TEST_CASE(storage_messages_reject_unknown_tags)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder { buffer };
    MUST(encoder.encode(u8 { 255 }));
    auto data = buffer.take_data();
    FixedMemoryStream stream { data.span() };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decoder.decode<StorageRequest>().is_error());
    MUST(stream.seek(0));
    EXPECT(decoder.decode<StorageResult>().is_error());
}
