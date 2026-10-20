/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGC/Weak.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Extensions/API/Event.h>
#include <LibWeb/Extensions/ExtensionDocumentContext.h>
#include <LibWeb/Extensions/ExtensionSet.h>
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWebCommon/Extensions/MessageSender.h>

namespace {

class TestPageClient final : public Web::PageClient {
    GC_CELL(TestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(TestPageClient);

public:
    virtual Web::PageId id() const override { return Web::PageId { 1 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }

    struct SentMessage {
        String extension_id;
        u64 request_id;
        Web::Extensions::MessageSender sender;
        String json;
    };
    struct Reply {
        u64 dispatch_id;
        Optional<String> json;
    };
    virtual void page_did_send_extension_runtime_message(String const& extension_id, u64 request_id, Web::Extensions::MessageSender const& sender, String const& json, u64) override
    {
        sent_messages.append({ extension_id, request_id, sender, json });
    }
    virtual void page_did_reply_to_extension_runtime_message(u64 dispatch_id, Optional<String> const& json) override
    {
        replies.append({ dispatch_id, json });
    }
    virtual void page_did_create_extension_context(String const& extension_id, u64, Web::HTML::CrossProcessId, u64) override { created_contexts.append(extension_id); }
    virtual void page_did_destroy_extension_context(String const& extension_id, u64) override { destroyed_contexts.append(extension_id); }

    Vector<SentMessage> sent_messages;
    Vector<Reply> replies;
    Vector<String> created_contexts;
    Vector<String> destroyed_contexts;

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(TestPageClient);

// A page with one top-level document, whose window has a principal realm to run scripts in.
struct TestPage {
    TestPage()
    {
        static Web::Platform::FontPlugin font_plugin { false };
        static bool font_plugin_installed = false;
        if (!font_plugin_installed) {
            Web::Platform::FontPlugin::install(font_plugin);
            font_plugin_installed = true;
        }

        principal_realm = Web::Bindings::create_a_principal_javascript_realm();
        VERIFY(principal_realm.ptr());
        auto& vm = Web::Bindings::main_thread_vm();

        client = vm.heap().allocate<TestPageClient>();
        page = Web::Page::create(*client);
        client->m_page = page.ptr();

        auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(*page, nullptr, {});
        page->set_top_level_traversable(traversable);
        document = GC::Ref { *traversable->active_document() };
        browsing_context = GC::Ref { *document->browsing_context() };
        window = GC::Ref { *document->window() };
    }

    JS::Value run_in_principal_realm(Utf16View source)
    {
        auto& vm = Web::Bindings::main_thread_vm();
        auto script_or_error = JS::Script::parse(source, window->principal_realm());
        VERIFY(!script_or_error.is_error());
        auto result = vm.run(script_or_error.value());
        VERIFY(!result.is_throw_completion());
        return result.release_value();
    }

    // Each fixture leaves its principal realm's execution context pushed, and a microtask checkpoint needs an
    // empty stack, so the contexts step aside for the checkpoint and come back in order.
    void run_microtasks()
    {
        auto& vm = Web::Bindings::main_thread_vm();
        Vector<JS::ExecutionContext*> pushed_contexts;
        while (!vm.execution_context_stack().is_empty())
            pushed_contexts.append(vm.pop_execution_context());
        Web::HTML::main_thread_event_loop().perform_a_microtask_checkpoint();
        for (size_t i = pushed_contexts.size(); i > 0; --i)
            vm.push_execution_context(*pushed_contexts[i - 1]);
    }

    GC::Root<JS::Realm> principal_realm;
    GC::Ptr<TestPageClient> client;
    GC::Ptr<Web::Page> page;
    GC::Ptr<Web::DOM::Document> document;
    GC::Ptr<Web::HTML::BrowsingContext> browsing_context;
    GC::Ptr<Web::HTML::Window> window;
};

}

TEST_CASE(classic_script_parsed_in_extension_realm_runs_there)
{
    TestPage test_page;
    auto& vm = Web::Bindings::main_thread_vm();
    auto& window = *test_page.window;

    auto extension_world = vm.heap().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Extension);
    auto extension_execution_context = Web::Bindings::create_a_new_javascript_realm(
        vm,
        Web::HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Web::Bindings::create_global_object_wrapper(realm, GC::Ref { window });
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return test_page.browsing_context->window_proxy_for(*extension_world, realm)->object();
        });
    auto& extension_realm = *extension_execution_context->realm;
    auto intrinsics = extension_realm.create<Web::Bindings::Intrinsics>(extension_realm);
    extension_realm.set_host_defined(extension_realm.heap().allocate<Web::Bindings::HostDefined>(intrinsics, *extension_world, window.principal_realm()));
    Web::Bindings::cache_global_object_wrapper(extension_realm);
    MUST(Web::Bindings::initialize_window_web_interfaces(window, extension_realm));

    auto& principal_settings = Web::Bindings::principal_host_defined_environment_settings_object(window.principal_realm());
    auto script = Web::HTML::ClassicScript::create_in_realm("test.js", u"globalThis.__w = 1; window"sv, principal_settings, extension_realm, URL::URL {});
    EXPECT(script->script_record());

    auto completion = script->run();
    EXPECT(!completion.is_abrupt());
    EXPECT(completion.value().is_object());
    EXPECT(&completion.value().as_object().shape().realm() == &extension_realm);

    // The expando landed on the extension world's window wrapper, where the page's own world can't see it.
    auto extension_has_w = MUST(extension_realm.global_object().has_own_property("__w"_utf16_fly_string));
    auto principal_has_w = MUST(window.principal_realm().global_object().has_own_property("__w"_utf16_fly_string));
    EXPECT(extension_has_w);
    EXPECT(!principal_has_w);
    EXPECT_EQ(test_page.run_in_principal_realm(u"typeof window.__w"sv).as_string().utf16_string(), u"undefined"sv);

    // A script that fails to parse records a SyntaxError of the realm it was parsed in.
    auto broken = Web::HTML::ClassicScript::create_in_realm("broken.js", u"let ="sv, principal_settings, extension_realm, URL::URL {});
    EXPECT(!broken->script_record());
    EXPECT(broken->parse_error().is_object());
    EXPECT(&broken->parse_error().as_object().shape().realm() == &extension_realm);

    extension_world->detach();
}

static Web::Extensions::ExtensionDescriptor test_descriptor()
{
    return Web::Extensions::ExtensionDescriptor {
        .id = "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string,
        .name = "World Test"_string,
        .version = "1"_string,
        .manifest_version = 3,
        .manifest_json = "{\"name\":\"World Test\",\"version\":\"1\"}"_string,
        .extension_pages_csp = {},
        .web_accessible_resources = {},
        .content_scripts = {},
        .has_background = false,
        .background_document_html = {},
    };
}

TEST_CASE(extension_world_global_is_a_window_wrapper_for_the_same_window)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());

    EXPECT(&world->realm() != &test_page.window->principal_realm());
    EXPECT(Web::Bindings::impl_from<Web::HTML::Window>(&world->realm().global_object()) == test_page.window.ptr());
    EXPECT(Web::Bindings::host_defined_wrapper_world(world->realm()).type() == Web::Bindings::WrapperWorld::Type::Extension);
    EXPECT(&Web::Bindings::principal_host_defined_environment_settings_object(world->realm()).realm() == &test_page.window->principal_realm());
    EXPECT_EQ(world->extension_id(), "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"sv);

    world->detach();
}

TEST_CASE(extension_world_sees_dom_through_its_own_wrappers)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());

    auto completion = world->run_script("a.js", u"document.body.setAttribute('data-x', '1'); document.body"sv, URL::URL {});
    EXPECT(!completion.is_abrupt());
    EXPECT(completion.value().is_object());
    EXPECT(&completion.value().as_object().shape().realm() == &world->realm());
    EXPECT(Web::Bindings::impl_from<Web::DOM::Node>(&completion.value().as_object()) == test_page.document->body());
    EXPECT_EQ(test_page.document->body()->get_attribute("data-x"_utf16_fly_string), u"1"sv);

    auto from_page = test_page.run_in_principal_realm(u"document.body.getAttribute('data-x')"sv);
    EXPECT_EQ(from_page.as_string().utf16_string(), u"1"sv);

    world->detach();
}

TEST_CASE(page_expandos_are_invisible_to_the_extension_world)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());

    test_page.run_in_principal_realm(u"window.pageSecret = 1; document.body.pageExpando = 2;"sv);
    auto seen_from_extension = world->run_script("b.js", u"typeof window.pageSecret + ' ' + typeof document.body.pageExpando"sv, URL::URL {});
    EXPECT_EQ(seen_from_extension.value().as_string().utf16_string(), u"undefined undefined"sv);

    (void)world->run_script("c.js", u"window.extensionSecret = 1; document.body.extensionExpando = 2;"sv, URL::URL {});
    auto seen_from_page = test_page.run_in_principal_realm(u"typeof window.extensionSecret + ' ' + typeof document.body.extensionExpando"sv);
    EXPECT_EQ(seen_from_page.as_string().utf16_string(), u"undefined undefined"sv);

    world->detach();
}

TEST_CASE(namespace_has_browser_and_chrome_as_one_object)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());

    auto same_object = world->run_script("d.js", u"browser === chrome && typeof browser.runtime === 'object'"sv, URL::URL {});
    EXPECT(same_object.value().is_boolean() && same_object.value().as_bool());

    auto id = world->run_script("e.js", u"browser.runtime.id"sv, URL::URL {});
    EXPECT_EQ(id.value().as_string().utf16_string(), u"0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"sv);

    auto url = world->run_script("f.js", u"browser.runtime.getURL('a/b.png') + ' ' + browser.runtime.getURL('/c')"sv, URL::URL {});
    EXPECT_EQ(url.value().as_string().utf16_string(), u"ladybird-extension://0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11/a/b.png ladybird-extension://0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11/c"sv);

    auto manifest_name = world->run_script("g.js", u"browser.runtime.getManifest().name"sv, URL::URL {});
    EXPECT_EQ(manifest_name.value().as_string().utf16_string(), u"World Test"sv);

    auto has_events = world->run_script("h.js", u"typeof browser.runtime.onMessage.addListener === 'function' && browser.runtime.lastError === undefined"sv, URL::URL {});
    EXPECT(has_events.value().is_boolean() && has_events.value().as_bool());

    // The page's own world never sees the namespace.
    auto from_page = test_page.run_in_principal_realm(u"typeof browser + ' ' + typeof chrome"sv);
    EXPECT_EQ(from_page.as_string().utf16_string(), u"undefined undefined"sv);

    world->detach();
}

TEST_CASE(document_owns_one_world_per_extension_until_detached)
{
    TestPage test_page;
    auto descriptor = test_descriptor();

    auto& first = test_page.document->ensure_extension_world(descriptor);
    auto& again = test_page.document->ensure_extension_world(descriptor);
    EXPECT(&first == &again);
    EXPECT(!first.is_detached());

    auto other = descriptor;
    other.id = "11111111-2222-3333-4444-555555555555"_string;
    auto& second = test_page.document->ensure_extension_world(other);
    EXPECT(&second != &first);
    EXPECT_EQ(test_page.document->extension_worlds().size(), 2u);

    GC::Root<Web::Extensions::ExtensionWorld> kept_alive { first };
    test_page.document->detach_extension_worlds();
    EXPECT(kept_alive->is_detached());
    EXPECT_EQ(test_page.document->extension_worlds().size(), 0u);

    // Running a script in a detached world is refused rather than touching a torn-down realm.
    auto completion = kept_alive->run_script("i.js", u"1"sv, URL::URL {});
    EXPECT(completion.is_abrupt());
}

static Web::Extensions::MessageSender test_sender()
{
    return Web::Extensions::MessageSender {
        .extension_id = "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string,
        .url = "ladybird-extension://0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11/_generated_background_page.html"_string,
        .from_background = true,
    };
}

TEST_CASE(send_message_notifies_the_page_client_and_resolves_on_reply)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());
    EXPECT_EQ(test_page.client->created_contexts, (Vector { "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string }));

    (void)world->run_script("send.js", u"globalThis.__reply = 'pending'; browser.runtime.sendMessage({ type: 'ping', n: 1 }).then(r => { globalThis.__reply = r; });"sv, URL::URL {});
    EXPECT_EQ(test_page.client->sent_messages.size(), 1u);
    auto const& sent = test_page.client->sent_messages[0];
    EXPECT_EQ(sent.extension_id, "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"sv);
    EXPECT_EQ(sent.json, "{\"type\":\"ping\",\"n\":1}"sv);
    EXPECT(!sent.sender.from_background);
    EXPECT(sent.sender.url.has_value());

    test_page.page->complete_extension_runtime_message(sent.request_id, "{\"answer\":\"pong\"}"_string);
    test_page.run_microtasks();
    auto reply = world->run_script("read.js", u"globalThis.__reply.answer"sv, URL::URL {});
    EXPECT_EQ(reply.value().as_string().utf16_string(), u"pong"sv);

    // No reply from anyone resolves the promise with undefined.
    (void)world->run_script("send2.js", u"globalThis.__reply = 'pending'; browser.runtime.sendMessage('x').then(r => { globalThis.__reply = String(r); });"sv, URL::URL {});
    test_page.page->complete_extension_runtime_message(test_page.client->sent_messages[1].request_id, {});
    test_page.run_microtasks();
    auto undefined_reply = world->run_script("read2.js", u"globalThis.__reply"sv, URL::URL {});
    EXPECT_EQ(undefined_reply.value().as_string().utf16_string(), u"undefined"sv);

    // A message that can't be serialized is rejected before anything is sent.
    (void)world->run_script("send3.js", u"globalThis.__error = ''; browser.runtime.sendMessage(1n).catch(e => { globalThis.__error = e.name; });"sv, URL::URL {});
    test_page.run_microtasks();
    EXPECT_EQ(test_page.client->sent_messages.size(), 2u);
    auto error = world->run_script("read3.js", u"globalThis.__error"sv, URL::URL {});
    EXPECT_EQ(error.value().as_string().utf16_string(), u"TypeError"sv);

    world->detach();
    EXPECT_EQ(test_page.client->destroyed_contexts, (Vector { "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string }));
}

TEST_CASE(dispatch_runs_listeners_and_collects_the_reply)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());
    auto& page = *test_page.page;
    auto& replies = test_page.client->replies;
    auto extension_id = "0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11"_string;

    // No listener: an immediate empty reply.
    page.dispatch_extension_runtime_message(extension_id, 1, test_sender(), "{\"type\":\"a\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 1u);
    EXPECT_EQ(replies[0].dispatch_id, 1u);
    EXPECT(!replies[0].json.has_value());

    // A listener answers with sendResponse, and sees the message and the sender.
    (void)world->run_script("l1.js", u"globalThis.l1 = (message, sender, sendResponse) => { sendResponse(`sync:${message.type}:${sender.id}:${sender.url}`); }; browser.runtime.onMessage.addListener(globalThis.l1);"sv, URL::URL {});
    page.dispatch_extension_runtime_message(extension_id, 2, test_sender(), "{\"type\":\"b\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 2u);
    EXPECT_EQ(replies[1].json, "\"sync:b:0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11:ladybird-extension://0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11/_generated_background_page.html\""sv);

    // A value a listener merely returns is not a reply; the channel closes with an empty one.
    (void)world->run_script("l1b.js", u"browser.runtime.onMessage.removeListener(globalThis.l1); globalThis.l1b = (m) => 'ignored:' + m.type; browser.runtime.onMessage.addListener(globalThis.l1b);"sv, URL::URL {});
    page.dispatch_extension_runtime_message(extension_id, 20, test_sender(), "{\"type\":\"b2\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 3u);
    EXPECT(!replies[2].json.has_value());
    replies.remove(2);

    // A listener that calls sendResponse synchronously with an object.
    (void)world->run_script("l2.js", u"browser.runtime.onMessage.removeListener(globalThis.l1b); globalThis.l2 = (m, s, sendResponse) => { sendResponse({ ok: m.type }); }; browser.runtime.onMessage.addListener(globalThis.l2);"sv, URL::URL {});
    page.dispatch_extension_runtime_message(extension_id, 3, test_sender(), "{\"type\":\"c\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 3u);
    EXPECT_EQ(replies[2].json, "{\"ok\":\"c\"}"sv);

    // A listener that returns true and calls sendResponse later.
    (void)world->run_script("l3.js", u"browser.runtime.onMessage.removeListener(globalThis.l2); globalThis.l3 = (m, s, sendResponse) => { if (m.type !== 'd') return; Promise.resolve().then(() => sendResponse('later')); return true; }; browser.runtime.onMessage.addListener(globalThis.l3);"sv, URL::URL {});
    page.dispatch_extension_runtime_message(extension_id, 4, test_sender(), "{\"type\":\"d\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 3u);
    test_page.run_microtasks();
    EXPECT_EQ(replies.size(), 4u);
    EXPECT_EQ(replies[3].json, "\"later\""sv);

    // A listener that returns a promise replies with what it resolves to.
    (void)world->run_script("l4.js", u"browser.runtime.onMessage.removeListener(globalThis.l3); globalThis.l4 = (m) => { if (m.type !== 'e') return; return Promise.resolve({ promised: true }); }; browser.runtime.onMessage.addListener(globalThis.l4);"sv, URL::URL {});
    page.dispatch_extension_runtime_message(extension_id, 5, test_sender(), "{\"type\":\"e\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 4u);
    test_page.run_microtasks();
    EXPECT_EQ(replies.size(), 5u);
    EXPECT_EQ(replies[4].json, "{\"promised\":true}"sv);

    // Listeners that return nothing for a message leave an empty reply once they have all run.
    page.dispatch_extension_runtime_message(extension_id, 6, test_sender(), "{\"type\":\"z\"}"_string, world->context_id());
    EXPECT_EQ(replies.size(), 6u);
    EXPECT(!replies[5].json.has_value());

    // A message for an extension with no world here is answered empty too.
    page.dispatch_extension_runtime_message("11111111-2222-3333-4444-555555555555"_string, 7, test_sender(), "1"_string, world->context_id());
    EXPECT_EQ(replies.size(), 7u);
    EXPECT(!replies[6].json.has_value());

    world->detach();
}

NEVER_INLINE static auto snapshot_of_removed_listener(JS::Realm& realm, GC::Weak<JS::FunctionObject>& weak)
{
    auto list = realm.heap().allocate<Web::Extensions::EventListenerList>();
    auto listener = JS::NativeFunction::create(realm, "removed"_utf16_fly_string, [](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        return JS::js_undefined();
    });
    weak = listener;
    list->add(*listener);
    auto snapshot = list->listeners();
    list->clear();
    return snapshot;
}

TEST_CASE(listener_snapshot_retains_removed_functions_during_collection)
{
    TestPage test_page;
    GC::Weak<JS::FunctionObject> listener;
    auto snapshot = snapshot_of_removed_listener(test_page.window->principal_realm(), listener);
    Web::Bindings::main_thread_vm().heap().collect_garbage();
    EXPECT(listener);
    if (listener)
        EXPECT(snapshot[0].ptr() == listener.ptr().ptr());
}

TEST_CASE(dispatch_counts_the_snapshot_it_invokes)
{
    TestPage test_page;
    auto first = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());
    for (auto world : { first }) {
        auto result = world->run_script("listeners.js", uR"(
            function listener() {
                document.dispatchEvent(new Event("remove-runtime-listeners"));
            }
            const second = () => {};
            browser.runtime.onMessage.addListener(listener);
            browser.runtime.onMessage.addListener(second);
            document.addEventListener("remove-runtime-listeners", () => {
                browser.runtime.onMessage.removeListener(listener);
                browser.runtime.onMessage.removeListener(second);
            });
        )"sv,
            URL::URL {});
        EXPECT(!result.is_abrupt());
    }
    test_page.page->dispatch_extension_runtime_message(test_descriptor().id, 80, test_sender(), "null"_string, first->context_id());
    EXPECT_EQ(test_page.client->replies.size(), 1u);
    first->detach();
}

static NEVER_INLINE void scrub_extension_test_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
}

NEVER_INLINE static GC::Weak<Web::DOM::Document> destroy_document_with_optional_extension_namespace(bool install_extension, bool save_response = false, bool pending_send = false)
{
    TestPage test_page;
    auto document = test_page.document;
    if (install_extension) {
        Web::Extensions::ExtensionSet::the().set_extensions({ test_descriptor() });
        document->set_url(URL::Parser::basic_parse("ladybird-extension://0f5c2a3e-9d3b-4c1a-8f7e-2b6d1c0a9e11/page.html"sv).value());
        auto context = Web::Extensions::ExtensionDocumentContext::create_if_extension_document(*document);
        VERIFY(context);
        if (save_response) {
            test_page.run_in_principal_realm(u"browser.runtime.onMessage.addListener((m, s, respond) => { window.savedResponse = respond; });"sv);
            test_page.page->dispatch_extension_runtime_message(test_descriptor().id, 90, test_sender(), "null"_string, context->context_id());
        }
        if (pending_send)
            test_page.run_in_principal_realm(u"browser.runtime.sendMessage(42);"sv);
        context->detach();
        Web::Extensions::ExtensionSet::the().set_extensions({});
    }
    document->destroy();
    return GC::Weak<Web::DOM::Document> { document };
}

TEST_CASE(detached_extension_document_is_collectible)
{
    auto ordinary_document = destroy_document_with_optional_extension_namespace(false);
    auto extension_document = destroy_document_with_optional_extension_namespace(true);
    auto document_with_response = destroy_document_with_optional_extension_namespace(true, true);
    scrub_extension_test_stack();
    Web::Bindings::main_thread_vm().heap().collect_garbage();
    EXPECT(!ordinary_document);
    EXPECT(!extension_document);
    EXPECT(!document_with_response);
}

NEVER_INLINE static GC::Weak<Web::Extensions::EventListenerList> unreachable_api_event_cycle(JS::Realm& realm)
{
    auto listeners = realm.heap().allocate<Web::Extensions::EventListenerList>();
    auto event = Web::Extensions::create_event(realm, listeners);
    auto listener = JS::NativeFunction::create(realm, "cycle"_utf16_fly_string, [event](JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        return event;
    });
    listeners->add(listener);
    return GC::Weak<Web::Extensions::EventListenerList> { listeners };
}

TEST_CASE(unreachable_api_event_cycle_is_collectible)
{
    TestPage test_page;
    auto listeners = unreachable_api_event_cycle(test_page.window->principal_realm());
    scrub_extension_test_stack();
    Web::Bindings::main_thread_vm().heap().collect_garbage();
    EXPECT(!listeners);
}

TEST_CASE(detached_sender_releases_pending_promise_roots)
{
    auto document = destroy_document_with_optional_extension_namespace(true, false, true);
    scrub_extension_test_stack();
    Web::Bindings::main_thread_vm().heap().collect_garbage();
    EXPECT(!document);
}

TEST_CASE(detached_receiver_declines_an_asynchronous_message)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());
    auto result = world->run_script("listener.js", u"browser.runtime.onMessage.addListener(() => true);"sv, URL::URL {});
    EXPECT(!result.is_abrupt());
    test_page.page->dispatch_extension_runtime_message(test_descriptor().id, 91, test_sender(), "null"_string, world->context_id());
    EXPECT(test_page.client->replies.is_empty());
    world->detach();
    EXPECT_EQ(test_page.client->replies.size(), 1u);
}

TEST_CASE(disabled_and_replaced_extensions_revoke_live_contexts)
{
    TestPage test_page;
    auto descriptor = test_descriptor();
    auto unrelated = descriptor;
    unrelated.id = "11111111-2222-3333-4444-555555555555"_string;
    auto& extensions = Web::Extensions::ExtensionSet::the();
    extensions.set_extensions({ descriptor, unrelated });
    GC::Ref world = test_page.document->ensure_extension_world(descriptor);
    GC::Ref other = test_page.document->ensure_extension_world(unrelated);
    auto get_url = GC::make_root(world->run_script("save.js", u"browser.runtime.getURL"sv, {}).value().as_function());
    auto get_manifest = GC::make_root(world->run_script("save.js", u"browser.runtime.getManifest"sv, {}).value().as_function());
    auto add_listener = GC::make_root(world->run_script("save.js", u"browser.runtime.onMessage.addListener"sv, {}).value().as_function());
    auto function = GC::Ref { world->run_script("callback.js", u"() => 42"sv, {}).value().as_function() };
    auto callback = world->realm().heap().allocate<Web::WebIDL::CallbackType>(*function, world->realm());

    (void)world->run_script("pending.js", u"globalThis.marker = []; new Promise(resolve => { globalThis.resolveLater = resolve; }).then(() => marker.push(42));"sv, {});
    auto resolve_later = GC::make_root(world->run_script("save.js", u"resolveLater"sv, {}).value().as_function());
    auto marker = GC::make_root(world->run_script("save.js", u"marker"sv, {}).value().as_object());

    extensions.set_extensions({ unrelated });
    EXPECT(world->is_detached());
    EXPECT(!other->is_detached());
    auto& vm = Web::Bindings::main_thread_vm();
    EXPECT(JS::call(vm, *get_url, JS::js_undefined()).is_error());
    EXPECT(JS::call(vm, *get_manifest, JS::js_undefined()).is_error());
    EXPECT(JS::call(vm, *add_listener, JS::js_undefined(), function).is_error());
    auto completion = Web::WebIDL::invoke_callback(*callback, {}, Web::WebIDL::ExceptionBehavior::Rethrow, {});
    EXPECT(!completion.is_abrupt());
    EXPECT(completion.value().is_undefined());
    EXPECT(!JS::call(vm, *resolve_later, JS::js_undefined()).is_error());
    test_page.run_microtasks();
    EXPECT_EQ(MUST(marker->get(vm.names.length)).as_i32(), 0);

    extensions.set_extensions({ descriptor, unrelated });
    GC::Ref replacement = test_page.document->ensure_extension_world(descriptor);
    EXPECT(replacement.ptr() != world.ptr());
    descriptor.name = "Replaced"_string;
    extensions.set_extensions({ descriptor, unrelated });
    EXPECT(replacement->is_detached());
    EXPECT(!other->is_detached());
    world->detach();
    replacement->detach();
    other->detach();
    extensions.set_extensions({});
}

TEST_CASE(send_message_normalizes_arguments_and_scopes_callback_errors)
{
    TestPage test_page;
    auto world = Web::Extensions::ExtensionWorld::create(*test_page.document, test_descriptor());
    auto result = world->run_script("callbacks.js", uR"JS(
        globalThis.results = [];
        globalThis.returns = [];
        const callback = value => results.push([value, browser.runtime.lastError === undefined]);
        returns.push(browser.runtime.sendMessage({ n: 1 }, callback));
        returns.push(browser.runtime.sendMessage({ n: 2 }, {}, callback));
        returns.push(browser.runtime.sendMessage(browser.runtime.id, { n: 3 }, callback));
        returns.push(browser.runtime.sendMessage(browser.runtime.id, { n: 4 }, {}, callback));
        returns.push(browser.runtime.sendMessage(null, { n: 5 }, null, callback));
        browser.runtime.sendMessage({ n: 6 }, undefined).then(callback);
        globalThis.failure = false;
        const returned = browser.runtime.sendMessage(1n, value => {
            failure = value === undefined && typeof browser.runtime.lastError.message === 'string';
        });
        returns.push(returned);
        if (returned instanceof Promise) returned.catch(() => {});
        globalThis.primitiveFailure = false;
        browser.runtime.sendMessage({ toJSON() { throw 'bad message'; } }, () => {
            primitiveFailure = typeof browser.runtime.lastError.message === 'string';
        });
        globalThis.externalFailure = false;
        browser.runtime.sendMessage('another-extension', 1, {}, () => {
            externalFailure = typeof browser.runtime.lastError.message === 'string';
        });
    )JS"sv,
        URL::URL {});
    EXPECT(!result.is_abrupt());
    EXPECT_EQ(test_page.client->sent_messages.size(), 6u);
    auto messages = test_page.client->sent_messages;
    for (size_t i = 0; i < messages.size(); ++i) {
        EXPECT_EQ(messages[i].json, MUST(String::formatted("{{\"n\":{}}}", i + 1)));
        test_page.page->complete_extension_runtime_message(messages[i].request_id, "42"_string);
    }
    test_page.run_microtasks();
    auto check = world->run_script("check.js", u"JSON.stringify([returns.every(value => value === undefined), results, failure, primitiveFailure, externalFailure, browser.runtime.lastError === undefined])"sv, URL::URL {});
    EXPECT_EQ(check.value().as_string().utf16_string(), u"[true,[[42,true],[42,true],[42,true],[42,true],[42,true],[42,true]],true,true,true,true]"sv);
    world->detach();
}
