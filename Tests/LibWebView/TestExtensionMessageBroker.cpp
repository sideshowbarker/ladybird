/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>

using Web::Extensions::MessageSender;
using WebView::ExtensionMessageBroker;

namespace {

// A stand-in for a WebContent page: records what the broker dispatches to it and what it completes.
struct FakeEndpoint final : public ExtensionMessageBroker::Endpoint {
    struct Dispatch {
        String extension_id;
        u64 dispatch_id;
        MessageSender sender;
        String json;
        u64 context_id;
    };
    struct Completion {
        u64 request_id;
        Optional<String> json;
        Web::Extensions::MessageResult result;
    };

    virtual void dispatch_extension_runtime_message(String const& extension_id, u64 dispatch_id, MessageSender const& sender, String const& json, u64 context_id) override
    {
        dispatches.append({ extension_id, dispatch_id, sender, json, context_id });
    }
    virtual void complete_extension_runtime_message(u64 request_id, Web::Extensions::MessageResult const& result) override
    {
        completions.append({ request_id, result.value.has<Web::Extensions::MessageResult::Reply>() ? Optional<String> { result.value.get<Web::Extensions::MessageResult::Reply>().json } : OptionalNone {}, result });
    }

    bool storage_permission { true };
    Vector<Web::Extensions::StorageResult> storage_replies;
    Vector<String> storage_events;
    virtual bool extension_has_permission(String const&, StringView permission) const override { return permission == "storage"sv && storage_permission; }
    virtual void complete_extension_storage(u64, Web::Extensions::StorageResult const& result) override { storage_replies.append(result); }
    virtual void dispatch_extension_storage_changed(String const&, String const& changes) override { storage_events.append(changes); }

    Vector<Dispatch> dispatches;
    Vector<Completion> completions;
    bool authorized { true };
    u64 revision { 1 };
    bool content_script { false };

    virtual Optional<u64> extension_revision(String const&) const override
    {
        return authorized ? Optional<u64> { revision } : OptionalNone {};
    }

    virtual Optional<MessageSender> extension_message_sender(String const& extension_id, Web::HTML::CrossProcessId document_id) const override
    {
        if (!authorized || document_id != Web::HTML::CrossProcessId { 1, 1 })
            return {};
        return MessageSender { .extension_id = extension_id, .url = content_script ? "https://example.com/"_string : MUST(String::formatted("ladybird-extension://{}/page.html", extension_id)), .from_background = false };
    }
};

}

static MessageSender sender_for(StringView id)
{
    return MessageSender { .extension_id = MUST(String::from_utf8(id)), .url = "https://example.com/"_string, .from_background = false };
}

static constexpr auto x = "11111111-1111-1111-1111-111111111111"sv;
static constexpr auto y = "22222222-2222-2222-2222-222222222222"sv;

TEST_CASE(sender_fields_come_from_the_endpoint)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(a, id, 1, { 1, 1 }, 1);
    broker.context_created(b, id, 1, { 1, 1 }, 1);
    a.content_script = true;
    auto forged = sender_for(y);
    forged.url = "https://forged.test/"_string;
    forged.from_background = true;
    broker.send(a, id, 1, forged, "1"_string, 1);
    EXPECT_EQ(b.dispatches.size(), 1u);
    EXPECT_EQ(b.dispatches[0].sender.extension_id, x);
    EXPECT_EQ(b.dispatches[0].sender.url, "https://example.com/"sv);
    EXPECT(!b.dispatches[0].sender.from_background);
}

TEST_CASE(unauthorized_endpoints_cannot_register_or_send)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(a, id, 1, { 1, 1 }, 1);
    b.authorized = false;
    broker.context_created(b, id, 1, { 1, 1 }, 1);
    broker.send(a, id, 1, sender_for(x), "1"_string, 1);
    EXPECT(b.dispatches.is_empty());
    broker.send(b, id, 2, sender_for(x), "1"_string, 1);
    EXPECT(a.dispatches.is_empty());
}

TEST_CASE(routes_to_every_other_extension_page_context)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b, c, d;
    a.content_script = true;
    broker.context_created(a, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);
    broker.context_created(b, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);
    broker.context_created(c, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);
    broker.context_created(d, MUST(String::from_utf8(y)), 1, { 1, 1 }, 1);

    broker.send(a, MUST(String::from_utf8(x)), 7, sender_for(x), "{\"hi\":1}"_string, 1);

    EXPECT_EQ(a.dispatches.size(), 0u);
    EXPECT_EQ(b.dispatches.size(), 1u);
    EXPECT_EQ(c.dispatches.size(), 1u);
    EXPECT_EQ(d.dispatches.size(), 0u);
    EXPECT_EQ(b.dispatches[0].extension_id, x);
    EXPECT_EQ(b.dispatches[0].json, "{\"hi\":1}"sv);
    EXPECT_EQ(b.dispatches[0].sender.url, "https://example.com/"sv);
    EXPECT_NE(b.dispatches[0].dispatch_id, c.dispatches[0].dispatch_id);
    EXPECT_EQ(a.completions.size(), 0u);
}

TEST_CASE(first_defined_reply_wins)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b, c;
    for (auto* endpoint : { &a, &b, &c })
        broker.context_created(*endpoint, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);

    broker.send(a, MUST(String::from_utf8(x)), 1, sender_for(x), "1"_string, 1);
    broker.reply(c, c.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "\"from c\""_string });
    EXPECT_EQ(a.completions.size(), 1u);
    EXPECT_EQ(a.completions[0].request_id, 1u);
    EXPECT_EQ(a.completions[0].json, "\"from c\""sv);

    // A later reply changes nothing.
    broker.reply(b, b.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "\"from b\""_string });
    EXPECT_EQ(a.completions.size(), 1u);

    // A reply with an unknown dispatch id is ignored.
    broker.reply(b, 999999, Web::Extensions::MessageResult::Reply { "\"stray\""_string });
    EXPECT_EQ(a.completions.size(), 1u);
}

TEST_CASE(no_targets_rejects_immediately)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a;
    broker.context_created(a, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);

    broker.send(a, MUST(String::from_utf8(x)), 3, sender_for(x), "1"_string, 1);
    EXPECT_EQ(a.completions.size(), 1u);
    EXPECT_EQ(a.completions[0].request_id, 3u);
    EXPECT(a.completions[0].result.value.has<Web::Extensions::MessageResult::Error>());
    EXPECT(!a.completions[0].json.has_value());

    // A context of a different extension isn't a target either.
    FakeEndpoint other;
    broker.context_created(other, MUST(String::from_utf8(y)), 1, { 1, 1 }, 1);
    broker.send(a, MUST(String::from_utf8(x)), 4, sender_for(x), "1"_string, 1);
    EXPECT_EQ(a.completions.size(), 2u);
    EXPECT(!a.completions[1].json.has_value());
    EXPECT_EQ(other.dispatches.size(), 0u);
}

TEST_CASE(every_target_declining_means_nobody_replied)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b, c;
    for (auto* endpoint : { &a, &b, &c })
        broker.context_created(*endpoint, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);

    broker.send(a, MUST(String::from_utf8(x)), 5, sender_for(x), "1"_string, 1);
    broker.reply(b, b.dispatches[0].dispatch_id, {});
    EXPECT_EQ(a.completions.size(), 0u);
    broker.reply(c, c.dispatches[0].dispatch_id, {});
    EXPECT_EQ(a.completions.size(), 1u);
    EXPECT(!a.completions[0].json.has_value());
}

TEST_CASE(destroyed_target_counts_as_declined)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b, c;
    for (auto* endpoint : { &a, &b, &c })
        broker.context_created(*endpoint, MUST(String::from_utf8(x)), 1, { 1, 1 }, 1);

    broker.send(a, MUST(String::from_utf8(x)), 6, sender_for(x), "1"_string, 1);
    broker.endpoint_gone(b);
    EXPECT_EQ(a.completions.size(), 0u);
    broker.reply(c, c.dispatches[0].dispatch_id, {});
    EXPECT_EQ(a.completions.size(), 1u);
    EXPECT(!a.completions[0].json.has_value());

    // The gone endpoint gets nothing more, and a source that is gone gets no completion.
    broker.send(c, MUST(String::from_utf8(x)), 8, sender_for(x), "1"_string, 1);
    EXPECT_EQ(b.dispatches.size(), 1u);
    EXPECT_EQ(a.dispatches.size(), 1u);
    broker.endpoint_gone(c);
    broker.reply(a, a.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "\"late\""_string });
    EXPECT_EQ(c.completions.size(), 0u);
}

TEST_CASE(contexts_on_one_endpoint_have_independent_lifetimes)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(a, id, 1, { 1, 1 }, 1);
    broker.context_created(b, id, 1, { 1, 1 }, 1);
    broker.context_created(b, id, 2, { 1, 1 }, 1);

    broker.send(a, id, 9, sender_for(x), "1"_string, 1);
    EXPECT_EQ(b.dispatches.size(), 2u);

    broker.context_destroyed(b, id, 1);
    EXPECT(a.completions.is_empty());
    broker.send(a, id, 10, sender_for(x), "1"_string, 1);
    EXPECT_EQ(b.dispatches.size(), 3u);
    EXPECT_EQ(b.dispatches[2].context_id, 2u);

    broker.context_destroyed(b, id, 2);
    EXPECT_EQ(a.completions.size(), 2u);
    broker.send(a, id, 11, sender_for(x), "1"_string, 1);
    EXPECT_EQ(b.dispatches.size(), 3u);
    EXPECT_EQ(a.completions.size(), 3u);
}

TEST_CASE(wrong_endpoint_cannot_consume_a_dispatch)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, target, unrelated;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.context_created(target, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 100, sender_for(x), "null"_string, 1);
    auto dispatch_id = target.dispatches[0].dispatch_id;
    broker.reply(unrelated, dispatch_id, Web::Extensions::MessageResult::Reply { "99"_string });
    EXPECT(source.completions.is_empty());
    broker.reply(target, dispatch_id, Web::Extensions::MessageResult::Reply { "42"_string });
    EXPECT_EQ(source.completions.size(), 1u);
    if (!source.completions.is_empty())
        EXPECT_EQ(source.completions[0].json, "42"sv);

    broker.send(source, id, 101, sender_for(x), "null"_string, 1);
    dispatch_id = target.dispatches[1].dispatch_id;
    broker.reply(unrelated, dispatch_id, {});
    broker.reply(target, dispatch_id, {});
    EXPECT_EQ(source.completions.size(), 2u);
}

TEST_CASE(destroying_a_receiver_context_declines_its_pending_dispatch)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, target;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.context_created(target, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 110, sender_for(x), "null"_string, 1);
    broker.context_destroyed(target, id, 1);
    EXPECT_EQ(source.completions.size(), 1u);
}

TEST_CASE(destroying_a_sender_context_drops_its_pending_request)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, target;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.context_created(target, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 111, sender_for(x), "null"_string, 1);
    broker.context_destroyed(source, id, 1);
    broker.reply(target, target.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "42"_string });
    EXPECT(source.completions.is_empty());
}

TEST_CASE(context_identity_is_scoped_to_its_endpoint_and_document)
{
    ExtensionMessageBroker broker;
    FakeEndpoint a, b;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(a, id, 1, { 1, 1 }, 1);
    broker.context_created(a, id, 2, { 1, 1 }, 1);
    broker.context_created(b, id, 3, { 1, 2 }, 1);
    broker.send(a, id, 120, sender_for(x), "1"_string, 1);
    EXPECT_EQ(a.dispatches.size(), 1u);
    EXPECT_EQ(a.dispatches[0].context_id, 2u);
    EXPECT(b.dispatches.is_empty());
    broker.context_destroyed(a, id, 1);
    broker.reply(a, a.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "2"_string });
    EXPECT(a.completions.is_empty());

    broker.context_created(b, id, 1, { 1, 1 }, 1);
    broker.send(b, id, 121, sender_for(x), "1"_string, 1);
    EXPECT_EQ(a.dispatches.size(), 2u);
    EXPECT_EQ(a.dispatches[1].context_id, 2u);
    broker.context_destroyed(a, id, 2);
    EXPECT_EQ(b.completions.size(), 1u);
}

TEST_CASE(replaced_instances_revoke_registered_contexts)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, target;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.context_created(target, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 130, sender_for(x), "1"_string, 1);
    target.revision = 2;
    broker.reconcile_contexts();
    EXPECT_EQ(source.completions.size(), 1u);
    broker.context_created(target, id, 2, { 1, 1 }, 1);
    broker.send(source, id, 131, sender_for(x), "1"_string, 1);
    EXPECT_EQ(target.dispatches.size(), 1u);
    EXPECT_EQ(source.completions.size(), 2u);
    broker.context_created(target, id, 3, { 1, 1 }, 2);
    broker.send(source, id, 132, sender_for(x), "1"_string, 1);
    EXPECT_EQ(target.dispatches.size(), 2u);
    EXPECT_EQ(target.dispatches[1].context_id, 3u);
}

TEST_CASE(replies_keep_their_request_identity_when_dispatch_maps_shrink)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, target;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.context_created(target, id, 1, { 1, 1 }, 1);
    for (u64 request = 1; request <= 256; ++request)
        broker.send(source, id, request, sender_for(x), "null"_string, 1);
    for (size_t i = 0; i < target.dispatches.size(); ++i)
        broker.reply(target, target.dispatches[i].dispatch_id, Web::Extensions::MessageResult::Reply { MUST(String::formatted("{}", i + 1)) });
    EXPECT_EQ(source.completions.size(), 256u);
    for (auto const& completion : source.completions)
        EXPECT_EQ(completion.json, MUST(String::formatted("{}", completion.request_id)));
}

TEST_CASE(runtime_messages_never_target_content_scripts)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, content, page;
    source.content_script = true;
    content.content_script = true;
    auto id = MUST(String::from_utf8(x));
    for (auto* endpoint : { &source, &content, &page })
        broker.context_created(*endpoint, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 1, sender_for(x), "1"_string, 1);
    EXPECT(content.dispatches.is_empty());
    EXPECT_EQ(page.dispatches.size(), 1u);
    broker.reply(page, page.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "2"_string });
    EXPECT_EQ(source.completions.size(), 1u);
    broker.send(page, id, 2, sender_for(x), "1"_string, 1);
    EXPECT(content.dispatches.is_empty());
    EXPECT(source.dispatches.is_empty());
    EXPECT_EQ(page.completions.size(), 1u);
}

TEST_CASE(messages_wait_for_background_startup_and_settle_on_failure)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, background;
    source.content_script = true;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(source, id, 1, { 1, 1 }, 1);
    broker.background_starting(id, 1);
    broker.send(source, id, 1, sender_for(x), "1"_string, 1);
    EXPECT(source.completions.is_empty());
    broker.context_created(background, id, 1, { 1, 1 }, 1);
    EXPECT(background.dispatches.is_empty());
    broker.background_finished(id, 1, true);
    EXPECT_EQ(background.dispatches.size(), 1u);
    if (!background.dispatches.is_empty())
        broker.reply(background, background.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "2"_string });
    EXPECT_EQ(source.completions.size(), 1u);
    EXPECT_EQ(source.completions[0].json, "2"sv);

    broker.background_starting(id, 1);
    broker.send(source, id, 2, sender_for(x), "3"_string, 1);
    EXPECT_EQ(background.dispatches.size(), 1u);
    broker.background_finished(id, 1, false);
    EXPECT_EQ(source.completions.size(), 2u);
    if (source.completions.size() == 2)
        EXPECT(!source.completions[1].json.has_value());

    broker.background_starting(id, 1);
    broker.send(source, id, 3, sender_for(x), "4"_string, 1);
    broker.context_destroyed(source, id, 1);
    broker.background_finished(id, 1, true);
    EXPECT_EQ(source.completions.size(), 2u);
    EXPECT_EQ(background.dispatches.size(), 1u);
}

TEST_CASE(listener_presence_is_distinct_from_a_declined_reply)
{
    ExtensionMessageBroker broker;
    FakeEndpoint source, first, second;
    auto id = MUST(String::from_utf8(x));
    for (auto* endpoint : { &source, &first, &second })
        broker.context_created(*endpoint, id, 1, { 1, 1 }, 1);
    broker.send(source, id, 1, sender_for(x), "null"_string, 1);
    broker.reply(first, first.dispatches.last().dispatch_id, Web::Extensions::MessageResult::NoListener {});
    EXPECT(source.completions.is_empty());
    broker.reply(second, second.dispatches.last().dispatch_id, Web::Extensions::MessageResult::NoListener {});
    EXPECT(source.completions.last().result.value.has<Web::Extensions::MessageResult::Error>());
    EXPECT_EQ(source.completions.last().result.value.get<Web::Extensions::MessageResult::Error>().message, "Could not establish connection. Receiving end does not exist."sv);

    broker.send(source, id, 2, sender_for(x), "null"_string, 1);
    broker.reply(first, first.dispatches.last().dispatch_id, Web::Extensions::MessageResult::NoReply {});
    broker.reply(second, second.dispatches.last().dispatch_id, Web::Extensions::MessageResult::NoListener {});
    EXPECT(source.completions.last().result.value.has<Web::Extensions::MessageResult::NoReply>());
}

namespace {

struct StorageFixture {
    Core::EventLoop loop;
    ByteString root { LexicalPath::join(Core::StandardPaths::tempfile_directory(), AK::generate_random_uuid()).string() };
    WebView::ExtensionStorageStore store { root };
    ExtensionMessageBroker broker;
    FakeEndpoint a, b, other;
    String id;
    StorageFixture()
        : id(MUST(String::from_utf8(x)))
    {
        MUST(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes));
        broker.context_created(a, id, 1, { 1, 1 }, 1);
        broker.context_created(b, id, 2, { 1, 1 }, 1);
        broker.context_created(other, MUST(String::from_utf8(y)), 3, { 1, 1 }, 1);
    }
    ~StorageFixture() { MUST(FileSystem::remove(root, FileSystem::RecursionMode::Allowed)); }
    void drain()
    {
        bool done = false;
        store.get(id, {}, [&](auto) { done = true; });
        loop.spin_until([&] { return done; });
    }
    void set(FakeEndpoint& endpoint, u64 context_id = 1)
    {
        broker.request_storage(endpoint, context_id, 1, { .operation = Web::Extensions::StorageRequest::Operation::Set, .keys = {}, .items_json = R"({"key":1})"_string }, store);
    }
};

}

TEST_CASE(storage_permission_is_enforced)
{
    StorageFixture f;
    f.a.storage_permission = false;
    f.set(f.a);
    f.drain();
    EXPECT_EQ(f.a.storage_replies.size(), 1u);
    if (!f.a.storage_replies.is_empty()) {
        EXPECT(f.a.storage_replies[0].type == Web::Extensions::StorageResult::Type::Error);
        EXPECT_EQ(f.a.storage_replies[0].json_or_error, "Missing the storage permission"sv);
    }
    EXPECT(f.a.storage_events.is_empty());
}

TEST_CASE(storage_request_from_detached_or_forged_context_is_dropped)
{
    StorageFixture f;
    f.set(f.a, 2);
    f.broker.context_destroyed(f.a, f.id, 1);
    f.set(f.a);
    f.b.revision = 2;
    f.set(f.b, 2);
    f.drain();
    EXPECT(f.a.storage_replies.is_empty());
    EXPECT(f.b.storage_replies.is_empty());
    EXPECT(f.other.storage_events.is_empty());
    EXPECT(!FileSystem::exists(LexicalPath::join(f.root, "extensions"sv).string()));
}

TEST_CASE(storage_changes_include_writer_and_only_same_extension)
{
    StorageFixture f;
    f.set(f.a);
    f.drain();
    EXPECT_EQ(f.a.storage_replies.size(), 1u);
    EXPECT_EQ(f.a.storage_events.size(), 1u);
    EXPECT_EQ(f.b.storage_events.size(), 1u);
    EXPECT(f.other.storage_events.is_empty());
    if (!f.a.storage_events.is_empty())
        EXPECT_EQ(f.a.storage_events[0], R"({"key":{"newValue":1}})"sv);
    f.set(f.a);
    f.drain();
    EXPECT_EQ(f.a.storage_events.size(), 1u);
    EXPECT_EQ(f.b.storage_events.size(), 1u);
}

TEST_CASE(storage_detach_during_write_drops_reply_but_not_other_events)
{
    StorageFixture f;
    f.set(f.a);
    f.broker.endpoint_gone(f.a);
    f.drain();
    EXPECT(f.a.storage_replies.is_empty());
    EXPECT(f.a.storage_events.is_empty());
    EXPECT_EQ(f.b.storage_events.size(), 1u);
}
