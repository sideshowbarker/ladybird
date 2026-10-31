/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionPrivacySettings.h>

using WebView::ExtensionPrivacySettings;

static constexpr auto prediction = "network.networkPredictionEnabled"sv;

static ExtensionPrivacySettings settings_for(Vector<String> const& install_order)
{
    return ExtensionPrivacySettings { [install_order] { return install_order; } };
}

static String level(ExtensionPrivacySettings const& settings, StringView extension_id, StringView name = prediction)
{
    return settings.get(extension_id, name).get_string("levelOfControl"sv).value();
}

TEST_CASE(defaults_are_controllable)
{
    auto settings = settings_for({ "a"_string });
    EXPECT_EQ(level(settings, "a"sv), "controllable_by_this_extension"sv);
    EXPECT_EQ(settings.get("a"sv, prediction).get_bool("value"sv), true);
    EXPECT_EQ(settings.get("a"sv, "network.webRTCIPHandlingPolicy"sv).get_string("value"sv), "default"_string);
    EXPECT(settings.network_prediction_enabled());
}

TEST_CASE(set_makes_it_controlled_by_this_extension)
{
    auto settings = settings_for({ "a"_string, "b"_string });
    EXPECT(settings.set("a"_string, prediction, false));
    EXPECT_EQ(level(settings, "a"sv), "controlled_by_this_extension"sv);
    // An extension installed later may still take it over.
    EXPECT_EQ(level(settings, "b"sv), "controllable_by_this_extension"sv);
    EXPECT(!settings.network_prediction_enabled());
}

TEST_CASE(last_installed_extension_wins)
{
    auto settings = settings_for({ "a"_string, "b"_string });
    EXPECT(settings.set("b"_string, prediction, false));
    EXPECT(!settings.set("a"_string, prediction, true));
    EXPECT(!settings.network_prediction_enabled());
    EXPECT_EQ(level(settings, "a"sv), "controlled_by_other_extensions"sv);
    EXPECT_EQ(level(settings, "b"sv), "controlled_by_this_extension"sv);
}

TEST_CASE(clear_falls_back_to_the_next_setter)
{
    auto settings = settings_for({ "a"_string, "b"_string });
    settings.set("a"_string, prediction, false);
    settings.set("b"_string, prediction, true);
    EXPECT(settings.network_prediction_enabled());
    EXPECT(settings.clear("b"sv, prediction));
    EXPECT(!settings.network_prediction_enabled());
    EXPECT(!settings.clear("b"sv, prediction));
}

TEST_CASE(removing_the_setter_restores_prediction)
{
    auto settings = settings_for({ "a"_string });
    Vector<String> changes;
    settings.on_effective_value_change = [&](StringView name) { changes.append(MUST(String::from_utf8(name))); };
    settings.set("a"_string, prediction, false);
    settings.set("a"_string, prediction, false);
    settings.remove_extension("a"sv);
    EXPECT(settings.network_prediction_enabled());
    EXPECT_EQ(changes, (Vector { "network.networkPredictionEnabled"_string, "network.networkPredictionEnabled"_string }));
}

TEST_CASE(hyperlink_auditing_reads_false_after_set)
{
    auto settings = settings_for({ "a"_string });
    EXPECT(!settings.set("a"_string, "websites.hyperlinkAuditingEnabled"sv, true));
    auto details = settings.get("a"sv, "websites.hyperlinkAuditingEnabled"sv);
    EXPECT_EQ(details.get_bool("value"sv), false);
    EXPECT_EQ(details.get_string("levelOfControl"sv), "not_controllable"_string);
}

TEST_CASE(accepted_settings_have_no_effect_on_prediction)
{
    auto settings = settings_for({ "a"_string });
    settings.set("a"_string, "network.webRTCIPHandlingPolicy"sv, "disable_non_proxied_udp"_string);
    settings.set("a"_string, "network.peerConnectionEnabled"sv, false);
    EXPECT(settings.network_prediction_enabled());
    EXPECT_EQ(settings.get("a"sv, "network.webRTCIPHandlingPolicy"sv).get_string("value"sv), "disable_non_proxied_udp"_string);
}

// The registry forgets a removed extension before the store hears of it, so the store can't recompute the value it
// had in effect from the install order alone.
TEST_CASE(removing_an_uninstalled_setter_restores_prediction)
{
    Vector<String> install_order { "a"_string, "b"_string };
    ExtensionPrivacySettings settings { [&] { return install_order; } };
    Vector<String> changes;
    settings.on_effective_value_change = [&](StringView name) { changes.append(MUST(String::from_utf8(name))); };
    settings.set("a"_string, prediction, true);
    settings.set("b"_string, prediction, false);
    changes.clear();

    install_order = { "a"_string };
    settings.remove_extension("b"sv);
    EXPECT(settings.network_prediction_enabled());
    EXPECT_EQ(changes, (Vector { "network.networkPredictionEnabled"_string }));
    EXPECT_EQ(level(settings, "a"sv), "controlled_by_this_extension"sv);
}

// An extension that loses the privacy permission, or is disabled, reloaded, or removed, loses the values it set.
TEST_CASE(retaining_only_kept_extensions_drops_the_others_values)
{
    auto settings = settings_for({ "a"_string, "b"_string });
    Vector<String> changes;
    settings.on_effective_value_change = [&](StringView name) { changes.append(MUST(String::from_utf8(name))); };
    settings.set("a"_string, prediction, false);
    settings.set("b"_string, "network.peerConnectionEnabled"sv, false);
    changes.clear();

    settings.retain_extensions([](StringView id) { return id == "b"sv; });
    EXPECT(settings.network_prediction_enabled());
    EXPECT_EQ(changes, (Vector { "network.networkPredictionEnabled"_string }));
    EXPECT_EQ(settings.get("b"sv, "network.peerConnectionEnabled"sv).get_bool("value"sv), false);
    EXPECT_EQ(settings.setters(), (Vector { "b"_string }));
}
