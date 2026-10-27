/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/ExtensionPopupPosition.h>

using Ladybird::extension_popup_position;

static QRect const screen { 0, 0, 1000, 800 };

TEST_CASE(popup_opens_below_its_button)
{
    // The popup's top-left corner meets the button's bottom-left corner.
    EXPECT_EQ(extension_popup_position({ 100, 40, 34, 34 }, { 300, 200 }, screen), QPoint(100, 74));
}

TEST_CASE(popup_stays_on_screen)
{
    // A button at the screen's right edge gets a popup shifted left, so that it ends at the screen's edge.
    EXPECT_EQ(extension_popup_position({ 960, 40, 34, 34 }, { 300, 200 }, screen), QPoint(700, 74));
    // A popup that wouldn't fit below its button near the bottom goes above it.
    EXPECT_EQ(extension_popup_position({ 100, 700, 34, 34 }, { 300, 200 }, screen), QPoint(100, 500));
    // One wider than the screen starts at the screen's left edge.
    EXPECT_EQ(extension_popup_position({ 100, 40, 34, 34 }, { 1200, 200 }, screen), QPoint(0, 74));
}
