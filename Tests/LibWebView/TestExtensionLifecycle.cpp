/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWebView/Extensions/ExtensionLifecycle.h>

TEST_CASE(an_extension_never_seen_is_installed)
{
    auto details = WebView::extension_installed_details({}, "1.0"_string);
    EXPECT_EQ(details->serialized(), "{\"reason\":\"install\"}"sv);
}

TEST_CASE(a_new_version_is_an_update_from_the_last_one)
{
    auto details = WebView::extension_installed_details("1.0"_string, "1.1"_string);
    EXPECT_EQ(details->serialized(), "{\"reason\":\"update\",\"previousVersion\":\"1.0\"}"sv);
}

TEST_CASE(the_same_version_is_neither)
{
    EXPECT(!WebView::extension_installed_details("1.0"_string, "1.0"_string).has_value());
}

TEST_CASE(only_an_extension_seen_before_hears_the_browser_start)
{
    EXPECT(WebView::extension_hears_browser_start("1.0"_string));
    EXPECT(!WebView::extension_hears_browser_start({}));
}
