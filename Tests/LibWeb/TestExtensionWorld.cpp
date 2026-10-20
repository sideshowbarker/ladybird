/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibGC/Weak.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
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
#include <LibWeb/Extensions/ExtensionWorld.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>

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

        auto client = vm.heap().allocate<TestPageClient>();
        page = Web::Page::create(client);
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

    GC::Root<JS::Realm> principal_realm;
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

static NEVER_INLINE void scrub_extension_test_stack()
{
    u8 volatile filler[8 * KiB];
    for (size_t i = 0; i < sizeof(filler); ++i)
        filler[i] = 0;
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
