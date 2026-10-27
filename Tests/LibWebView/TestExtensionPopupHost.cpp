/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibURL/Parser.h>
#include <LibWebView/Extensions/ExtensionPopupHost.h>

using namespace WebView;

struct FakeFrontend final : public ExtensionPopupHost::Frontend {
    virtual bool show_extension_popup(ExtensionPopupRequest const& request) override
    {
        shown.append({ request.extension_id, request.window_id });
        generations.append(request.generation);
        return true;
    }
    virtual void resize_extension_popup(i64 window_id, Gfx::IntSize size) override { resized.append({ window_id, size }); }
    virtual void close_extension_popup(i64 window_id) override { closed.append(window_id); }
    virtual ViewImplementation* extension_popup_view(i64) const override { return nullptr; }

    Vector<std::pair<String, i64>> shown;
    Vector<std::pair<i64, Gfx::IntSize>> resized;
    Vector<i64> closed;
    Vector<u64> generations;
};

static ExtensionPopupRequest request_for(StringView extension_id, i64 window_id)
{
    auto url = URL::Parser::basic_parse(MUST(String::formatted("ladybird-extension://{}/popup.html", extension_id))).release_value();
    return { MUST(String::from_utf8(extension_id)), window_id, window_id, url, 0 };
}

TEST_CASE(popup_size_is_clamped)
{
    EXPECT_EQ(clamp_extension_popup_size({ 10, 10 }), Gfx::IntSize(25, 25));
    EXPECT_EQ(clamp_extension_popup_size({ 900, 700 }), Gfx::IntSize(800, 600));
    EXPECT_EQ(clamp_extension_popup_size({ 300, 200 }), Gfx::IntSize(300, 200));
    EXPECT_EQ(clamp_extension_popup_size({ 10, 700 }), Gfx::IntSize(25, 600));

    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    host.content_size_changed(1, frontend.generations.last(), { 900, 10 });
    VERIFY(frontend.resized.size() == 1);
    EXPECT_EQ(frontend.resized[0].second, Gfx::IntSize(800, 25));
    // The same size again doesn't resize, and a window without a popup ignores sizes.
    host.content_size_changed(1, frontend.generations.last(), { 900, 10 });
    host.content_size_changed(2, frontend.generations.last(), { 300, 300 });
    EXPECT_EQ(frontend.resized.size(), 1u);
}

TEST_CASE(toggling_closes_an_open_popup)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    EXPECT(host.is_open(1));
    host.toggle(request_for("a"sv, 1));
    EXPECT(!host.is_open(1));
    EXPECT_EQ(frontend.shown.size(), 1u);
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));
}

TEST_CASE(another_extension_replaces_the_popup)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    host.toggle(request_for("b"sv, 1));
    EXPECT(host.is_open(1));
    EXPECT_EQ(host.extension_in(1), "b"sv);
    EXPECT_EQ(frontend.shown.size(), 2u);
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));
}

TEST_CASE(one_popup_per_window)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    host.toggle(request_for("a"sv, 2));
    EXPECT(host.is_open(1));
    EXPECT(host.is_open(2));
    host.close(1, frontend.generations[0], ExtensionPopupHost::CloseReason::FocusLost);
    EXPECT(!host.is_open(1));
    EXPECT(host.is_open(2));
    // Closing a closed popup does nothing.
    host.close(1, frontend.generations[0], ExtensionPopupHost::CloseReason::Escape);
    EXPECT_EQ(frontend.closed, (Vector<i64> { 1 }));
}

TEST_CASE(extension_removal_closes_its_popup)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    host.toggle(request_for("b"sv, 2));
    host.extension_gone("a"sv);
    EXPECT(!host.is_open(1));
    EXPECT(host.is_open(2));
}

TEST_CASE(a_replaced_popups_late_callbacks_dont_reach_its_replacement)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    auto first = frontend.generations.last();
    host.toggle(request_for("b"sv, 1));
    // A's view reports a size and closes itself after B replaced it; B stays open and unresized.
    host.content_size_changed(1, first, { 300, 300 });
    host.close(1, first, ExtensionPopupHost::CloseReason::WindowClose);
    EXPECT(host.is_open(1));
    EXPECT_EQ(host.extension_in(1), "b"sv);
    EXPECT(frontend.resized.is_empty());
}

TEST_CASE(a_popup_closes_with_the_tab_that_opened_it)
{
    FakeFrontend frontend;
    ExtensionPopupHost host(frontend);
    host.toggle(request_for("a"sv, 1));
    host.tab_closed(2);
    EXPECT(host.is_open(1));
    host.tab_closed(1);
    EXPECT(!host.is_open(1));
}
