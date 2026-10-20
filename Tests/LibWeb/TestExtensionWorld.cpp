/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
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
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLDocument.h>
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
