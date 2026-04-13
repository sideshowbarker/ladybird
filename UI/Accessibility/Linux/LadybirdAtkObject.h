/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>

#include <atk/atk.h>

namespace Ladybird {

// Callback for routing accessibility actions (press, focus, etc.) back to the view implementation.
using AccessibilityActionCallback = Function<void(i64 node_id, String action)>;

// Callback for structured text actions: set_caret_offset, set_selection, scroll_substring_to, etc.
// offset_start/end are Unicode character offsets; text carries additional payload.
using AccessibilityTextActionCallback
    = Function<void(i64 node_id, String action, i32 offset_start, i32 offset_end, String text)>;

// GObject/ATK type for wrapping a single AccessibilityNodeData node. Implements AtkObject with the full suite of
// ATK interfaces that Orca needs for web content navigation: AtkText, AtkHypertext, AtkDocument, AtkComponent,
// AtkAction, and AtkTableCell.
//
// Follows the same pattern as Chromium's AXPlatformNodeAuraLinux and Firefox's MaiAtkObject.

AtkObject* ladybird_atk_object_new(i64 node_id, WebView::AccessibilityTreeManager const* manager,
    AccessibilityActionCallback* action_callback = nullptr,
    AccessibilityTextActionCallback* text_action_callback = nullptr);

// Map a WAI-ARIA role string to an ATK role constant.
AtkRole ladybird_aria_role_to_atk_role(StringView role);

// Drop every cached AtkObject. Used by tests to ensure independence between fixtures; production
// code never needs this because the tree manager persists for the process lifetime.
void ladybird_atk_object_clear_cache();

// Drop cached AtkObject entries whose node_id no longer appears in the given tree. AtkBridge calls
// this on every tree update so the per-node cache doesn’t grow unbounded across page navigations.
void ladybird_atk_object_prune_cache(WebView::AccessibilityTreeManager const& manager);

}
