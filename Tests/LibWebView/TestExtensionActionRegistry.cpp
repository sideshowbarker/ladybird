/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/JsonArray.h>
#include <LibWebView/Extensions/ExtensionActionRegistry.h>

using namespace WebView;
using Property = ExtensionActionProperty;

static ExtensionManifest::Action action_with_defaults()
{
    ExtensionManifest::Action action;
    action.default_title = "Default title"_string;
    action.default_popup = "popup.html"_string;
    action.default_icon.append({ 16, "icons/16.png"_string });
    return action;
}

static ExtensionActionRegistry registry_with_one_extension()
{
    ExtensionActionRegistry registry;
    registry.set_extension("ext"_string, "Extension name"_string, action_with_defaults());
    return registry;
}

TEST_CASE(manifest_defaults_resolve)
{
    auto registry = registry_with_one_extension();
    auto state = registry.state("ext"sv, {});
    EXPECT_EQ(state.title, "Default title"sv);
    EXPECT_EQ(state.popup, "popup.html"sv);
    VERIFY(state.icon.size() == 1);
    EXPECT_EQ(state.icon[0].path, "icons/16.png"sv);
    EXPECT_EQ(state.badge_text, ""sv);
    EXPECT_EQ(state.badge_background_color, Gfx::Color(0xd9, 0, 0));
    EXPECT(state.enabled);

    // Without a manifest title, the button is titled by the extension's name.
    ExtensionActionRegistry untitled;
    untitled.set_extension("ext"_string, "Extension name"_string, {});
    EXPECT_EQ(untitled.state("ext"sv, {}).title, "Extension name"sv);
    EXPECT_EQ(untitled.state("ext"sv, {}).popup, ""sv);
}

TEST_CASE(per_tab_values_stay_on_their_tab)
{
    auto registry = registry_with_one_extension();
    registry.set("ext"sv, 1, Property::BadgeText, JsonValue { "3"sv });
    EXPECT_EQ(registry.state("ext"sv, 1).badge_text, "3"sv);
    EXPECT_EQ(registry.state("ext"sv, 2).badge_text, ""sv);
    EXPECT_EQ(registry.state("ext"sv, {}).badge_text, ""sv);
}

TEST_CASE(tab_value_wins_over_global)
{
    auto registry = registry_with_one_extension();
    registry.set("ext"sv, {}, Property::Title, JsonValue { "Global"sv });
    registry.set("ext"sv, 1, Property::Title, JsonValue { "Tab"sv });
    EXPECT_EQ(registry.state("ext"sv, 1).title, "Tab"sv);
    EXPECT_EQ(registry.state("ext"sv, 2).title, "Global"sv);
    EXPECT_EQ(registry.get("ext"sv, 1, Property::Title)->as_string(), "Tab"sv);
    EXPECT_EQ(registry.get("ext"sv, {}, Property::Title)->as_string(), "Global"sv);
}

TEST_CASE(clearing_a_tab_value_falls_back_to_global)
{
    auto registry = registry_with_one_extension();
    registry.set("ext"sv, {}, Property::BadgeText, JsonValue { "G"sv });
    registry.set("ext"sv, 1, Property::BadgeText, JsonValue { "T"sv });
    registry.set("ext"sv, 1, Property::BadgeText, {});
    EXPECT_EQ(registry.state("ext"sv, 1).badge_text, "G"sv);
    registry.set("ext"sv, {}, Property::BadgeText, {});
    EXPECT_EQ(registry.state("ext"sv, 1).badge_text, ""sv);
}

TEST_CASE(clear_tab_drops_only_that_tab)
{
    auto registry = registry_with_one_extension();
    registry.set("ext"sv, 1, Property::BadgeText, JsonValue { "1"sv });
    registry.set("ext"sv, 2, Property::BadgeText, JsonValue { "2"sv });
    registry.set("ext"sv, {}, Property::BadgeText, JsonValue { "G"sv });
    registry.clear_tab(1);
    EXPECT_EQ(registry.state("ext"sv, 1).badge_text, "G"sv);
    EXPECT_EQ(registry.state("ext"sv, 2).badge_text, "2"sv);
}

TEST_CASE(translucent_background_is_drawn_opaque)
{
    auto registry = registry_with_one_extension();
    JsonArray color;
    for (auto component : { 10, 20, 30, 0 })
        color.must_append(component);
    registry.set("ext"sv, {}, Property::BadgeBackgroundColor, JsonValue { color });
    EXPECT_EQ(registry.state("ext"sv, {}).badge_background_color, Gfx::Color(10, 20, 30, 255));
}

TEST_CASE(default_text_color_contrasts)
{
    EXPECT_EQ(default_badge_text_color(Gfx::Color(0xd9, 0, 0)), Gfx::Color::White);
    EXPECT_EQ(default_badge_text_color(Gfx::Color(0xff, 0xff, 0)), Gfx::Color::Black);
    EXPECT_EQ(default_badge_text_color(Gfx::Color(0, 0, 0)), Gfx::Color::White);

    auto registry = registry_with_one_extension();
    JsonArray yellow;
    for (auto component : { 255, 255, 0, 255 })
        yellow.must_append(component);
    registry.set("ext"sv, {}, Property::BadgeBackgroundColor, JsonValue { yellow });
    EXPECT_EQ(registry.state("ext"sv, {}).badge_text_color, Gfx::Color::Black);
}

TEST_CASE(removed_extension_drops_its_state)
{
    auto registry = registry_with_one_extension();
    registry.set("ext"sv, {}, Property::BadgeText, JsonValue { "9"sv });
    registry.remove_extension("ext"sv);
    EXPECT(!registry.has_action("ext"sv));
    EXPECT(registry.extensions_with_actions().is_empty());
    registry.set_extension("ext"_string, "Extension name"_string, action_with_defaults());
    EXPECT_EQ(registry.state("ext"sv, {}).badge_text, ""sv);
}

TEST_CASE(on_change_fires_with_the_tab)
{
    auto registry = registry_with_one_extension();
    Vector<Optional<i64>> changes;
    registry.on_change = [&](String const& extension_id, Optional<i64> tab_id) {
        EXPECT_EQ(extension_id, "ext"sv);
        changes.append(tab_id);
    };
    registry.set("ext"sv, 7, Property::BadgeText, JsonValue { "x"sv });
    registry.set("ext"sv, {}, Property::Enabled, JsonValue { false });
    VERIFY(changes.size() == 2);
    EXPECT_EQ(changes[0], 7);
    EXPECT(!changes[1].has_value());
    EXPECT(!registry.state("ext"sv, 7).enabled);
}

TEST_CASE(reloading_keeps_the_extensions_order)
{
    ExtensionActionRegistry registry;
    for (auto id : { "a"sv, "b"sv, "c"sv })
        registry.set_extension(MUST(String::from_utf8(id)), "Name"_string, {});
    registry.set_extension("a"_string, "Name"_string, {});
    EXPECT_EQ(registry.extensions_with_actions(), (Vector { "a"_string, "b"_string, "c"_string }));
}
