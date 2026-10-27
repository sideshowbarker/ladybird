/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibWebView/Extensions/ExtensionManifest.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class ExtensionActionProperty : u8 {
    Title,
    Icon,
    Popup,
    BadgeText,
    BadgeBackgroundColor,
    BadgeTextColor,
    Enabled,
};

static constexpr size_t extension_action_property_count = to_underlying(ExtensionActionProperty::Enabled) + 1;

// What an extension's toolbar button shows in one tab, with every default applied.
struct ExtensionActionState {
    String title;
    Vector<ExtensionManifest::IconPath> icon;
    String popup;
    String badge_text;
    Gfx::Color badge_background_color;
    Gfx::Color badge_text_color;
    bool enabled { true };
};

// Black or white, whichever contrasts more with the background by WCAG relative luminance; white on a tie.
WEBVIEW_API Gfx::Color default_badge_text_color(Gfx::Color background);

// The state of every extension's action, in the UI process. Each property has a global value and a value per tab;
// a tab's value wins over the global one, which wins over the manifest's default. Nothing here is persisted.
class WEBVIEW_API ExtensionActionRegistry {
public:
    void set_extension(String id, String name, ExtensionManifest::Action);
    void remove_extension(StringView id);
    bool has_action(StringView id) const;
    Vector<String> extensions_with_actions() const;

    // An empty value clears the property at that level. Values are as the action API passes them: strings for the
    // title, popup, and badge text; [r, g, b, a] arrays for colors; a size-to-path object or a path for the icon; and
    // a boolean for the enabled state.
    void set(StringView id, Optional<i64> tab_id, ExtensionActionProperty, Optional<JsonValue>);
    Optional<JsonValue> get(StringView id, Optional<i64> tab_id, ExtensionActionProperty) const;
    ExtensionActionState state(StringView id, Optional<i64> tab_id) const;

    // Drops every extension's values for a tab that navigated to another document or closed.
    void clear_tab(i64 tab_id);

    Function<void(String const& extension_id, Optional<i64> tab_id)> on_change;

private:
    struct PropertyValues {
        Optional<JsonValue> global;
        HashMap<i64, JsonValue> per_tab;
    };

    struct Entry {
        String id;
        String name;
        ExtensionManifest::Action action;
        Array<PropertyValues, extension_action_property_count> values;
    };

    Entry const* find(StringView id) const;
    Entry* find(StringView id);
    Optional<JsonValue> effective_value(Entry const&, Optional<i64> tab_id, ExtensionActionProperty) const;

    Vector<Entry> m_entries;
};

}
