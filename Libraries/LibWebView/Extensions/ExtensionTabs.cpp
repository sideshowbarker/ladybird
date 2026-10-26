/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionTabs.h>

namespace WebView {

static i64 s_next_extension_tab_id { 1 };
static i64 s_next_extension_window_id { 1 };

i64 allocate_extension_tab_id()
{
    return s_next_extension_tab_id++;
}

i64 allocate_extension_window_id()
{
    return s_next_extension_window_id++;
}

}
