/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionActionRegistry.h>
#include <UI/Qt/ExtensionActionButton.h>

#include <QApplication>

using Ladybird::ExtensionActionButton;

static WebView::ExtensionActionState state_with(StringView title, StringView badge, bool enabled = true)
{
    WebView::ExtensionActionState state;
    state.title = MUST(String::from_utf8(title));
    state.badge_text = MUST(String::from_utf8(badge));
    state.badge_background_color = Gfx::Color(0xd9, 0, 0);
    state.badge_text_color = Gfx::Color::White;
    state.enabled = enabled;
    return state;
}

TEST_CASE(tooltip_is_the_title)
{
    ExtensionActionButton button(nullptr);
    button.set_state(state_with("Web Specs Companion"sv, ""sv), {});
    EXPECT_EQ(button.toolTip(), QString("Web Specs Companion"));
    EXPECT_EQ(button.accessibleName(), QString("Web Specs Companion"));
}

TEST_CASE(badge_stays_within_the_button)
{
    ExtensionActionButton button(nullptr);
    button.set_state(state_with("Title"sv, ""sv), {});
    EXPECT(button.badge_rect().isEmpty());

    button.set_state(state_with("Title"sv, "3"sv), {});
    auto short_badge = button.badge_rect();
    EXPECT(!short_badge.isEmpty());

    // A long badge is clipped to the badge's maximum width, rather than cut at a number of characters.
    button.set_state(state_with("Title"sv, "1234567890"sv), {});
    auto long_badge = button.badge_rect();
    EXPECT(long_badge.width() > short_badge.width());
    EXPECT(long_badge.width() <= ExtensionActionButton::maximum_badge_width);
    EXPECT(long_badge.left() >= 0);
    EXPECT(long_badge.right() < button.width());
}

TEST_CASE(a_disabled_action_disables_the_button)
{
    ExtensionActionButton button(nullptr);
    button.set_state(state_with("Title"sv, ""sv, false), {});
    EXPECT(!button.isEnabled());
    button.set_state(state_with("Title"sv, ""sv, true), {});
    EXPECT(button.isEnabled());
}
