/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionSidePanelHost.h>

using namespace WebView;

// NB: The suites of this binary share one namespace, and the popup host's suite has a FakeFrontend of its own.
namespace {

// Two windows: window 1 holds tabs 10 (active) and 11; window 2 holds tab 20. Extension "p" may not show in window 2.
struct FakeFrontend final : public ExtensionSidePanelHost::Frontend {
    virtual bool show_extension_side_panel(ExtensionSidePanelRequest const& request) override
    {
        shown.append(request);
        return true;
    }
    virtual void navigate_extension_side_panel(i64 window_id, URL::URL const& url) override { navigated.append({ window_id, MUST(String::from_utf8(url.serialize_path())) }); }
    virtual void close_extension_side_panel(i64 window_id) override { closed.append(window_id); }
    virtual ViewImplementation* extension_side_panel_view(i64) const override { return nullptr; }
    virtual Optional<i64> extension_window_of_tab(i64 tab_id) const override
    {
        if (tab_id == 10 || tab_id == 11)
            return 1;
        if (tab_id == 20)
            return 2;
        return {};
    }
    virtual Optional<i64> extension_active_tab_of_window(i64 window_id) const override
    {
        if (window_id == 1)
            return active_in_window_1;
        if (window_id == 2)
            return 20;
        return {};
    }
    virtual bool extension_may_show_in_window(StringView extension_id, i64 window_id) const override { return !(extension_id == "p"sv && window_id == 2); }

    i64 active_in_window_1 { 10 };
    Vector<ExtensionSidePanelRequest> shown;
    Vector<std::pair<i64, String>> navigated;
    Vector<i64> closed;
};

static ExtensionManifest::SidePanel panel_declaration(StringView path, Optional<String> title = {})
{
    return { .path = MUST(String::from_utf8(path)), .title = move(title), .icon = {}, .declared_as_sidebar_action = false };
}

static void add_extension(ExtensionSidePanelHost& host, StringView id)
{
    host.set_extension(MUST(String::from_utf8(id)), "Extension"_string, panel_declaration("panel.html"sv, "Panel"_string));
}

}

TEST_CASE(options_resolve_per_tab_over_global)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    EXPECT(host.has_side_panel("a"sv));
    EXPECT(!host.has_side_panel("b"sv));

    auto global = host.options("a"sv, {});
    EXPECT_EQ(global.path, "/panel.html"sv);
    EXPECT(global.enabled);

    host.set_options("a"sv, 10, "tab.html"_string, {});
    EXPECT_EQ(host.options("a"sv, 10).path, "/tab.html"sv);
    EXPECT_EQ(host.options("a"sv, 11).path, "/panel.html"sv);

    // A per-tab override leaves the fields it doesn't name alone, and clearing the path leaves the panel disabled there.
    host.set_options("a"sv, 10, {}, false);
    EXPECT_EQ(host.options("a"sv, 10).path, "/tab.html"sv);
    EXPECT(!host.options("a"sv, 10).enabled);
    host.set_options("a"sv, {}, "global.html"_string, {});
    EXPECT_EQ(host.options("a"sv, {}).path, "/global.html"sv);
    EXPECT_EQ(host.options("a"sv, 11).path, "/global.html"sv);

    // The options of an unknown extension are empty and disabled.
    EXPECT(!host.options("b"sv, {}).path.has_value());
    EXPECT(!host.options("b"sv, {}).enabled);
}

TEST_CASE(open_needs_an_id_a_panel_and_a_window_it_may_show_in)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    add_extension(host, "p"sv);

    EXPECT_EQ(host.open("a"sv, {}, {}).error(), "At least one of `tabId` and `windowId` must be provided"sv);
    EXPECT_EQ(host.open("a"sv, 7, {}).error(), "No window with id: 7."sv);
    EXPECT_EQ(host.open("a"sv, {}, 70).error(), "No tab with id: 70."sv);
    EXPECT_EQ(host.open("a"sv, 2, 10).error(), "The specified tab does not belong to the specified window."sv);
    EXPECT_EQ(host.open("p"sv, 2, {}).error(), "No window with id: 2."sv);

    host.set_options("a"sv, 10, {}, false);
    EXPECT_EQ(host.open("a"sv, {}, 10).error(), "No active side panel for tabId: 10"sv);
    host.set_options("a"sv, {}, {}, false);
    EXPECT_EQ(host.open("a"sv, 1, {}).error(), "No active side panel for windowId: 1"sv);
    EXPECT_EQ(host.open("b"sv, 1, {}).error(), "No active side panel for windowId: 1"sv);
    EXPECT(frontend.shown.is_empty());
}

TEST_CASE(open_shows_the_resolved_page_and_reports_it)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    Vector<String> opened;
    host.on_opened = [&](String const& id, i64 window_id, Optional<i64> tab_id, String const& path) {
        opened.append(MUST(String::formatted("{} {} {} {}", id, window_id, tab_id.value_or(-1), path)));
    };

    EXPECT(!host.open("a"sv, 1, {}).is_error());
    VERIFY(frontend.shown.size() == 1);
    EXPECT_EQ(frontend.shown[0].extension_id, "a"sv);
    EXPECT_EQ(frontend.shown[0].window_id, 1);
    EXPECT_EQ(frontend.shown[0].url.serialize_path(), "/panel.html"sv);
    EXPECT_EQ(frontend.shown[0].title, "Panel"sv);
    EXPECT(host.is_open(1));
    EXPECT(host.is_open("a"sv, 1));
    EXPECT(!host.is_open(2));
    EXPECT_EQ(host.extension_in(1), "a"sv);
    EXPECT_EQ(opened, (Vector { "a 1 -1 /panel.html"_string }));

    // Opening the same extension's panel again in the window shows nothing new.
    EXPECT(!host.open("a"sv, {}, 11).is_error());
    EXPECT_EQ(frontend.shown.size(), 1u);

    // A tab id resolves to its window, and the tab's own page.
    host.set_options("a"sv, 20, "tab.html"_string, {});
    EXPECT(!host.open("a"sv, {}, 20).is_error());
    VERIFY(frontend.shown.size() == 2);
    EXPECT_EQ(frontend.shown[1].window_id, 2);
    EXPECT_EQ(frontend.shown[1].url.serialize_path(), "/tab.html"sv);
    EXPECT_EQ(opened.last(), "a 2 20 /tab.html"sv);

    auto state = host.state(1);
    VERIFY(state.has_value());
    EXPECT_EQ(state->extension_id, "a"sv);
    EXPECT_EQ(state->url.serialize_path(), "/panel.html"sv);
    EXPECT(!host.state(3).has_value());
}

TEST_CASE(another_extension_replaces_the_panel_and_toggle_closes_it)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    add_extension(host, "b"sv);
    Vector<String> closed;
    host.on_closed = [&](String const& id, i64 window_id, Optional<i64>, String const& path) {
        closed.append(MUST(String::formatted("{} {} {}", id, window_id, path)));
    };

    EXPECT(!host.open("a"sv, 1, {}).is_error());
    EXPECT(!host.open("b"sv, 1, {}).is_error());
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));
    EXPECT_EQ(frontend.shown.size(), 2u);
    EXPECT_EQ(host.extension_in(1), "b"sv);
    EXPECT_EQ(closed, (Vector { "a 1 /panel.html"_string }));

    host.toggle("b"sv, 1);
    EXPECT(!host.is_open(1));
    EXPECT_EQ(frontend.closed.size(), 2u);
    host.toggle("b"sv, 1);
    EXPECT(host.is_open("b"sv, 1));

    // close() by API: the extension's own panel only.
    EXPECT(!host.close("a"sv, 1, {}).is_error());
    EXPECT(host.is_open("b"sv, 1));
    EXPECT(!host.close("b"sv, {}, 11).is_error());
    EXPECT(!host.is_open(1));
    EXPECT_EQ(host.close("b"sv, {}, {}).error(), "At least one of `tabId` and `windowId` must be provided"sv);
}

TEST_CASE(the_panel_follows_the_active_tab)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    EXPECT(!host.open("a"sv, 1, {}).is_error());

    // The same resolved page keeps the document.
    frontend.active_in_window_1 = 11;
    host.active_tab_changed(1, 11);
    EXPECT(frontend.navigated.is_empty());
    EXPECT(host.is_open(1));

    // A tab with a page of its own navigates the panel there, and back on leaving it.
    host.set_options("a"sv, 10, "tab.html"_string, {});
    frontend.active_in_window_1 = 10;
    host.active_tab_changed(1, 10);
    EXPECT_EQ(frontend.navigated, (Vector<std::pair<i64, String>> { { 1, "/tab.html"_string } }));
    EXPECT_EQ(host.state(1)->url.serialize_path(), "/tab.html"sv);
    frontend.active_in_window_1 = 11;
    host.active_tab_changed(1, 11);
    EXPECT_EQ(frontend.navigated.size(), 2u);
    EXPECT_EQ(frontend.navigated[1].second, "/panel.html"sv);

    // Setting the active tab's own page while the panel is open navigates at once; another tab's doesn't.
    host.set_options("a"sv, 11, "eleven.html"_string, {});
    EXPECT_EQ(frontend.navigated.size(), 3u);
    EXPECT_EQ(frontend.navigated[2].second, "/eleven.html"sv);
    host.set_options("a"sv, 10, "ten.html"_string, {});
    EXPECT_EQ(frontend.navigated.size(), 3u);

    // A tab where the panel is disabled closes it.
    host.set_options("a"sv, 10, {}, false);
    frontend.active_in_window_1 = 10;
    host.active_tab_changed(1, 10);
    EXPECT(!host.is_open(1));
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));
}

TEST_CASE(tab_overrides_go_with_the_tab)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    host.set_options("a"sv, 10, "side.html"_string, {}, ExtensionSidePanelHost::OverrideSource::SidePanel);
    host.set_options("a"sv, 11, "bar.html"_string, {}, ExtensionSidePanelHost::OverrideSource::SidebarAction);

    // A navigation clears what sidebarAction set for the tab, and keeps what sidePanel set.
    host.tab_navigated(10);
    host.tab_navigated(11);
    EXPECT_EQ(host.options("a"sv, 10).path, "/side.html"sv);
    EXPECT_EQ(host.options("a"sv, 11).path, "/panel.html"sv);

    // A closed tab loses its override, and an open panel showing it re-resolves.
    EXPECT(!host.open("a"sv, 1, {}).is_error());
    EXPECT_EQ(frontend.shown.last().url.serialize_path(), "/side.html"sv);
    host.tab_closed(10);
    EXPECT_EQ(host.options("a"sv, 10).path, "/panel.html"sv);
    frontend.active_in_window_1 = 11;
    host.active_tab_changed(1, 11);
    EXPECT_EQ(frontend.navigated.last().second, "/panel.html"sv);
}

TEST_CASE(removing_the_extension_closes_its_panels_and_forgets_its_options)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    add_extension(host, "b"sv);
    host.set_options("a"sv, {}, "custom.html"_string, {});
    host.set_opens_on_action_click("a"sv, true);
    EXPECT(host.opens_on_action_click("a"sv));
    EXPECT(!host.opens_on_action_click("b"sv));
    EXPECT(!host.open("a"sv, 1, {}).is_error());
    EXPECT(!host.open("b"sv, 2, {}).is_error());

    host.remove_extension("a"sv);
    EXPECT(!host.is_open(1));
    EXPECT(host.is_open("b"sv, 2));
    EXPECT(!host.has_side_panel("a"sv));
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));

    // Set again (re-enabled), the manifest's options are back.
    add_extension(host, "a"sv);
    EXPECT_EQ(host.options("a"sv, {}).path, "/panel.html"sv);
    EXPECT(!host.opens_on_action_click("a"sv));
}

TEST_CASE(title_and_icon_resolve_per_tab_over_global_over_manifest)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    EXPECT_EQ(host.title("a"sv, {}), "Panel"sv);
    EXPECT_EQ(host.title("a"sv, 10), "Panel"sv);
    host.set_title("a"sv, {}, "Global"_string);
    host.set_title("a"sv, 10, "Ten"_string);
    EXPECT_EQ(host.title("a"sv, {}), "Global"sv);
    EXPECT_EQ(host.title("a"sv, 10), "Ten"sv);
    EXPECT_EQ(host.title("a"sv, 11), "Global"sv);
    // The panel's header shows the active tab's title.
    EXPECT(!host.open("a"sv, 1, {}).is_error());
    EXPECT_EQ(frontend.shown.last().title, "Ten"sv);
    EXPECT_EQ(host.state(1)->title, "Ten"sv);
    // Null clears a level; the tab's override goes with the tab.
    host.set_title("a"sv, 10, {});
    EXPECT_EQ(host.title("a"sv, 10), "Global"sv);
    host.set_title("a"sv, {}, {});
    EXPECT_EQ(host.title("a"sv, 10), "Panel"sv);
    host.set_icon("a"sv, 10, Vector<ExtensionManifest::IconPath> { { 16, "ten.png"_string } });
    EXPECT_EQ(host.icon("a"sv, 10).size(), 1u);
    EXPECT(host.icon("a"sv, 11).is_empty());
    host.tab_closed(10);
    EXPECT(host.icon("a"sv, 10).is_empty());
}

TEST_CASE(a_closed_panels_late_callbacks_are_ignored)
{
    FakeFrontend frontend;
    ExtensionSidePanelHost host(frontend);
    add_extension(host, "a"sv);
    add_extension(host, "b"sv);
    EXPECT(!host.open("a"sv, 1, {}).is_error());
    auto first_generation = frontend.shown[0].generation;
    EXPECT(!host.open("b"sv, 1, {}).is_error());
    auto second_generation = frontend.shown[1].generation;
    EXPECT(first_generation != second_generation);

    // The replaced panel's view closing doesn't close its replacement.
    host.close(1, first_generation, ExtensionSidePanelHost::CloseReason::PageClosed);
    EXPECT(host.is_open("b"sv, 1));
    host.close(1, second_generation, ExtensionSidePanelHost::CloseReason::PageClosed);
    EXPECT(!host.is_open(1));

    // Closing a window with no panel does nothing.
    host.close(3, ExtensionSidePanelHost::CloseReason::WindowClose);
    EXPECT_EQ(frontend.closed.size(), 2u);
}
