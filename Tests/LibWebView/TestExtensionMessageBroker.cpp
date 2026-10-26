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

    struct PortEndpoint {
        u64 context_id;
        u64 channel_id;
        u64 port_id;
    };
    struct Connect : PortEndpoint {
        String name;
        MessageSender sender;
    };
    struct PortMessage : PortEndpoint {
        String json;
    };
    struct Disconnect {
        u64 context_id;
        u64 port_id;
        Optional<String> error;
    };
    Vector<Connect> connects;
    Vector<PortEndpoint> connected;
    Vector<PortMessage> port_messages;
    Vector<Disconnect> disconnected;
    Function<void()> on_connect;
    virtual void dispatch_extension_runtime_connect(u64 context, u64 channel, u64 port, String const& name, MessageSender const& sender) override
    {
        connects.append({ { context, channel, port }, name, sender });
        if (on_connect)
            on_connect();
    }
    virtual void complete_extension_runtime_connect(u64 context, u64 port, u64 channel, Optional<String> const& error) override
    {
        connected.append({ context, channel, port });
        if (error.has_value())
            disconnected.append({ context, port, error });
    }
    virtual void dispatch_extension_port_message(u64 context, u64 channel, u64 port, String const& json) override
    {
        port_messages.append({ { context, channel, port }, json });
    }
    virtual void dispatch_extension_port_disconnect(u64 context, u64 port, Optional<String> const& error) override
    {
        disconnected.append({ context, port, error });
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
    bool background { false };

    virtual Optional<u64> extension_revision(String const&) const override
    {
        return authorized ? Optional<u64> { revision } : OptionalNone {};
    }

    Optional<u64> view_id;
    Optional<i64> frame_id;
    Optional<String> tab_json;
    virtual Optional<u64> extension_view_id() const override { return view_id; }
    virtual Optional<i64> extension_frame_id(Web::HTML::CrossProcessId) const override { return frame_id; }
    virtual void annotate_extension_sender(MessageSender& sender, Web::HTML::CrossProcessId) const override
    {
        sender.tab_json = tab_json;
        sender.frame_id = frame_id;
    }

    virtual Optional<MessageSender> extension_message_sender(String const& extension_id, Web::HTML::CrossProcessId document_id) const override
    {
        if (!authorized || document_id != Web::HTML::CrossProcessId { 1, 1 })
            return {};
        return MessageSender { .extension_id = extension_id, .url = content_script ? "https://example.com/"_string : MUST(String::formatted("ladybird-extension://{}/page.html", extension_id)), .from_background = background };
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

TEST_CASE(current_contexts_say_whether_they_are_extension_documents)
{
    ExtensionMessageBroker broker;
    FakeEndpoint page, content;
    content.content_script = true;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(page, id, 1, { 1, 1 }, 1);
    broker.context_created(content, id, 1, { 1, 1 }, 1);
    EXPECT(broker.current_context(page, 1)->is_extension_document);
    EXPECT(!broker.current_context(content, 1)->is_extension_document);
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

namespace {

struct PortFixture {
    ExtensionMessageBroker broker;
    FakeEndpoint opener, first, second, foreign;
    String id;
    PortFixture()
        : id(MUST(String::from_utf8(x)))
    {
        first.background = true;
        for (auto* endpoint : { &opener, &first, &second })
            broker.context_created(*endpoint, id, 1, { 1, 1 }, 1);
        broker.context_created(foreign, MUST(String::from_utf8(y)), 1, { 1, 1 }, 1);
    }
    u64 connect()
    {
        broker.connect(opener, 1, 42, {}, "conversation"_string);
        return opener.connected.last().channel_id;
    }
    void ack(FakeEndpoint& endpoint, bool accepted = true)
    {
        broker.connect_ack(endpoint, 1, endpoint.connects.last().channel_id, accepted);
    }
    void disconnect(FakeEndpoint& endpoint)
    {
        auto port = endpoint.connects.last();
        broker.port_disconnect(endpoint, 1, port.channel_id, port.port_id);
    }
};

}

TEST_CASE(connect_reaches_every_page_but_the_opener)
{
    PortFixture f;
    FakeEndpoint content;
    content.content_script = true;
    f.broker.context_created(content, f.id, 1, { 1, 1 }, 1);
    f.broker.context_created(f.opener, f.id, 2, { 1, 1 }, 1);
    auto channel = f.connect();
    EXPECT_EQ(f.first.connects.size(), 1u);
    EXPECT_EQ(f.second.connects.size(), 1u);
    EXPECT_EQ(f.opener.connects.size(), 1u);
    EXPECT_EQ(f.opener.connects[0].context_id, 2u);
    EXPECT(content.connects.is_empty());
    EXPECT(f.foreign.connects.is_empty());
    EXPECT_EQ(f.first.connects[0].channel_id, channel);
    EXPECT_EQ(f.first.connects[0].name, "conversation"sv);
    EXPECT_EQ(f.first.connects[0].sender.extension_id, x);
    EXPECT_EQ(f.first.connects[0].sender.url, MUST(String::formatted("ladybird-extension://{}/page.html", x)));
}

TEST_CASE(no_listener_disconnects_the_opener_with_the_receiving_end_error)
{
    PortFixture f;
    f.connect();
    f.ack(f.first, false);
    EXPECT(f.opener.disconnected.is_empty());
    f.ack(f.second, false);
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT_EQ(f.opener.disconnected.last().error, "Could not establish connection. Receiving end does not exist."sv);
    EXPECT(!f.broker.has_open_channels(f.id));
}

TEST_CASE(other_extension_id_disconnects_with_the_receiving_end_error)
{
    PortFixture f;
    f.broker.connect(f.opener, 1, 42, MUST(String::from_utf8(y)), {});
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT_EQ(f.opener.disconnected.last().error, "Could not establish connection. Receiving end does not exist."sv);
    EXPECT(f.foreign.connects.is_empty());
    EXPECT(f.first.connects.is_empty());
}

TEST_CASE(messages_queue_until_every_target_acks)
{
    PortFixture f;
    auto channel = f.connect();
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    f.ack(f.first);
    f.broker.port_post_message(f.opener, 1, channel, 42, "2"_string);
    EXPECT(f.first.port_messages.is_empty());
    f.ack(f.second);
    for (auto* endpoint : { &f.first, &f.second }) {
        EXPECT_EQ(endpoint->port_messages.size(), 2u);
        EXPECT_EQ(endpoint->port_messages[0].json, "1"sv);
        EXPECT_EQ(endpoint->port_messages[1].json, "2"sv);
    }
    f.broker.port_post_message(f.first, 1, channel, f.first.connects[0].port_id, "3"_string);
    EXPECT_EQ(f.opener.port_messages.size(), 1u);
    EXPECT_EQ(f.second.port_messages.size(), 2u);
}

TEST_CASE(queue_overflow_closes_the_channel)
{
    PortFixture f;
    auto channel = f.connect();
    for (size_t i = 0; i < 1024; ++i)
        f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    EXPECT(f.opener.disconnected.is_empty());
    f.broker.port_post_message(f.opener, 1, channel, 42, "2"_string);
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT_EQ(f.opener.disconnected.last().error, "Port message queue is full"sv);
    f.ack(f.first);
    f.ack(f.second);
    EXPECT(f.first.port_messages.is_empty());
    EXPECT(!f.broker.has_open_channels(f.id));
}

TEST_CASE(opener_survives_while_a_receiver_remains)
{
    PortFixture f;
    auto channel = f.connect();
    f.ack(f.first);
    f.ack(f.second);
    f.disconnect(f.first);
    EXPECT(f.first.disconnected.is_empty());
    EXPECT(f.opener.disconnected.is_empty());
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    EXPECT(f.first.port_messages.is_empty());
    EXPECT_EQ(f.second.port_messages.size(), 1u);
    f.disconnect(f.second);
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT(!f.opener.disconnected.last().error.has_value());
}

TEST_CASE(opener_disconnect_reaches_every_receiver)
{
    PortFixture f;
    auto channel = f.connect();
    f.ack(f.first);
    f.ack(f.second);
    f.broker.port_disconnect(f.opener, 1, channel, 42);
    EXPECT(f.opener.disconnected.is_empty());
    EXPECT_EQ(f.first.disconnected.size(), 1u);
    EXPECT_EQ(f.second.disconnected.size(), 1u);
    EXPECT(!f.first.disconnected.last().error.has_value());
    EXPECT(!f.broker.has_open_channels(f.id));
}

TEST_CASE(endpoint_destruction_removes_only_its_side)
{
    PortFixture f;
    auto channel = f.connect();
    f.ack(f.first);
    f.ack(f.second);
    f.broker.endpoint_gone(f.first);
    EXPECT(f.opener.disconnected.is_empty());
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    EXPECT_EQ(f.second.port_messages.size(), 1u);
    EXPECT(f.first.port_messages.is_empty());
    f.broker.endpoint_gone(f.opener);
    EXPECT_EQ(f.second.disconnected.size(), 1u);
    EXPECT(f.first.disconnected.is_empty());
}

TEST_CASE(opener_detaching_while_opening_closes_the_channel)
{
    PortFixture f;
    auto detach = [&] { f.broker.context_destroyed(f.opener, f.id, 1); };
    f.first.on_connect = [detach] { detach(); };
    f.second.on_connect = [detach] { detach(); };
    f.connect();
    EXPECT_EQ(f.first.connects.size() + f.second.connects.size(), 1u);
    EXPECT_EQ(f.first.disconnected.size() + f.second.disconnected.size(), 1u);
    EXPECT(!f.broker.has_open_channels(f.id));
    EXPECT(f.opener.disconnected.is_empty());
}

TEST_CASE(foreign_channel_ids_are_ignored)
{
    PortFixture f;
    auto channel = f.connect();
    f.broker.connect_ack(f.foreign, 1, channel, true);
    f.broker.port_post_message(f.foreign, 1, channel, 42, "99"_string);
    f.broker.port_disconnect(f.foreign, 1, channel, 42);
    f.broker.port_disconnect(f.opener, 2, channel, 42);
    f.broker.port_disconnect(f.opener, 1, channel, 43);
    f.broker.port_disconnect(f.second, 1, channel, f.first.connects[0].port_id);
    EXPECT(f.opener.disconnected.is_empty());
    EXPECT(f.first.port_messages.is_empty());
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    f.ack(f.first);
    EXPECT(f.first.port_messages.is_empty());
    f.ack(f.second);
    EXPECT_EQ(f.first.port_messages.size(), 1u);
    EXPECT_EQ(f.second.port_messages.size(), 1u);
}

TEST_CASE(disable_closes_the_extensions_channels)
{
    PortFixture f;
    f.connect();
    f.broker.close_channels_for(f.id);
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT_EQ(f.first.disconnected.size(), 1u);
    EXPECT_EQ(f.second.disconnected.size(), 1u);
    EXPECT(!f.opener.disconnected.last().error.has_value());
    EXPECT(!f.broker.has_open_channels(f.id));
}

TEST_CASE(has_open_channels_tracks_the_background)
{
    PortFixture f;
    EXPECT(!f.broker.has_open_channels(f.id));
    f.connect();
    EXPECT(f.broker.has_open_channels(f.id));
    EXPECT(!f.broker.has_open_channels(MUST(String::from_utf8(y))));
    f.ack(f.first);
    f.ack(f.second);
    EXPECT(f.broker.has_open_channels(f.id));
    f.disconnect(f.first);
    EXPECT(!f.broker.has_open_channels(f.id));
    EXPECT(f.opener.disconnected.is_empty());
}

TEST_CASE(receiver_disconnect_during_onconnect_is_clean_and_drops_queued_messages)
{
    PortFixture f;
    auto channel = f.connect();
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    f.disconnect(f.first);
    f.ack(f.first);
    f.ack(f.second, false);
    EXPECT_EQ(f.opener.disconnected.size(), 1u);
    EXPECT(!f.opener.disconnected.last().error.has_value());
    EXPECT(f.first.port_messages.is_empty());
}

TEST_CASE(ports_wait_for_background_startup_and_handle_failure_or_detachment)
{
    PortFixture f;
    f.broker.background_starting(f.id, 1);
    auto channel = f.connect();
    f.broker.port_post_message(f.opener, 1, channel, 42, "1"_string);
    EXPECT(f.first.connects.is_empty());
    f.broker.background_finished(f.id, 1, true);
    f.ack(f.first);
    f.ack(f.second);
    EXPECT_EQ(f.first.port_messages.size(), 1u);
    f.broker.close_channels_for(f.id);
    f.broker.background_starting(f.id, 1);
    f.connect();
    f.broker.background_finished(f.id, 1, false);
    EXPECT_EQ(f.opener.disconnected.last().error, "Could not establish connection. Receiving end does not exist."sv);
    f.broker.background_starting(f.id, 1);
    f.connect();
    f.broker.context_destroyed(f.opener, f.id, 1);
    f.broker.background_finished(f.id, 1, true);
    EXPECT_EQ(f.first.connects.size(), 1u);
    EXPECT(!f.broker.has_open_channels(f.id));
}

TEST_CASE(tab_messages_reach_only_that_tabs_content_scripts)
{
    ExtensionMessageBroker broker;
    FakeEndpoint background, in_tab, elsewhere, page_in_tab;
    in_tab.content_script = true;
    in_tab.view_id = 7;
    elsewhere.content_script = true;
    elsewhere.view_id = 8;
    page_in_tab.view_id = 7;
    auto id = MUST(String::from_utf8(x));
    for (auto* endpoint : { &background, &in_tab, &elsewhere, &page_in_tab })
        broker.context_created(*endpoint, id, 1, { 1, 1 }, 1);
    broker.send_to_tab(background, id, 1, 1, 7, {}, "1"_string);
    EXPECT_EQ(in_tab.dispatches.size(), 1u);
    EXPECT(elsewhere.dispatches.is_empty());
    EXPECT(page_in_tab.dispatches.is_empty());
    broker.reply(in_tab, in_tab.dispatches[0].dispatch_id, Web::Extensions::MessageResult::Reply { "2"_string });
    EXPECT_EQ(background.completions.size(), 1u);
    EXPECT_EQ(background.completions[0].json, "2"_string);
}

TEST_CASE(tab_messages_can_target_one_frame)
{
    ExtensionMessageBroker broker;
    FakeEndpoint background, top, subframe;
    top.content_script = true;
    top.view_id = 7;
    top.frame_id = 0;
    subframe.content_script = true;
    subframe.view_id = 7;
    subframe.frame_id = 5;
    auto id = MUST(String::from_utf8(x));
    for (auto* endpoint : { &background, &top, &subframe })
        broker.context_created(*endpoint, id, 1, { 1, 1 }, 1);
    broker.send_to_tab(background, id, 1, 1, 7, 5, "1"_string);
    EXPECT(top.dispatches.is_empty());
    EXPECT_EQ(subframe.dispatches.size(), 1u);
    broker.send_to_tab(background, id, 2, 1, 7, {}, "1"_string);
    EXPECT_EQ(top.dispatches.size(), 1u);
    EXPECT_EQ(subframe.dispatches.size(), 2u);
}

TEST_CASE(tab_messages_with_no_receiver_reject)
{
    ExtensionMessageBroker broker;
    FakeEndpoint background, other_extension;
    other_extension.content_script = true;
    other_extension.view_id = 7;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(background, id, 1, { 1, 1 }, 1);
    broker.context_created(other_extension, MUST(String::from_utf8(y)), 1, { 1, 1 }, 1);
    broker.send_to_tab(background, id, 1, 1, 7, {}, "1"_string);
    broker.send_to_tab(background, id, 2, 1, 9, {}, "1"_string);
    EXPECT(other_extension.dispatches.is_empty());
    EXPECT_EQ(background.completions.size(), 2u);
    for (auto const& completion : background.completions)
        EXPECT_EQ(completion.result, Web::Extensions::MessageResult::no_receiver());
}

TEST_CASE(senders_carry_their_tab_and_frame)
{
    ExtensionMessageBroker broker;
    FakeEndpoint background, content;
    content.content_script = true;
    content.view_id = 7;
    content.frame_id = 0;
    content.tab_json = "{\"id\":3}"_string;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(background, id, 1, { 1, 1 }, 1);
    broker.context_created(content, id, 1, { 1, 1 }, 1);
    broker.send(content, id, 1, sender_for(x), "1"_string, 1);
    VERIFY(background.dispatches.size() == 1);
    EXPECT_EQ(background.dispatches[0].sender.tab_json, "{\"id\":3}"_string);
    EXPECT_EQ(background.dispatches[0].sender.frame_id, 0);
    broker.send_to_tab(background, id, 1, 1, 7, {}, "1"_string);
    VERIFY(content.dispatches.size() == 1);
    EXPECT(!content.dispatches[0].sender.tab_json.has_value());
}

TEST_CASE(content_scripts_cannot_send_tab_messages)
{
    ExtensionMessageBroker broker;
    FakeEndpoint content, other_content;
    content.content_script = true;
    content.view_id = 7;
    other_content.content_script = true;
    other_content.view_id = 8;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(content, id, 1, { 1, 1 }, 1);
    broker.context_created(other_content, id, 1, { 1, 1 }, 1);
    broker.send_to_tab(content, id, 1, 1, 8, {}, "1"_string);
    EXPECT(other_content.dispatches.is_empty());
    VERIFY(content.completions.size() == 1);
    EXPECT(content.completions[0].result.value.has<Web::Extensions::MessageResult::Error>());
}

TEST_CASE(a_negative_frame_id_means_every_frame)
{
    ExtensionMessageBroker broker;
    FakeEndpoint background, top;
    top.content_script = true;
    top.view_id = 7;
    top.frame_id = 0;
    auto id = MUST(String::from_utf8(x));
    broker.context_created(background, id, 1, { 1, 1 }, 1);
    broker.context_created(top, id, 1, { 1, 1 }, 1);
    broker.send_to_tab(background, id, 1, 1, 7, -1, "1"_string);
    EXPECT_EQ(top.dispatches.size(), 1u);
}
