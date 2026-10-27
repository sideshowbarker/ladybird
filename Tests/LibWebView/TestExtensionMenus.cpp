/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionMenus.h>

using WebView::ExtensionMenus;

static JsonObject item(JsonValue id)
{
    JsonObject object;
    object.set("id"sv, move(id));
    object.set("title"sv, "Item"_string);
    return object;
}

TEST_CASE(items_go_when_their_extension_reloads_or_goes_away)
{
    ExtensionMenus menus;
    MUST(menus.create("one"_string, 1, item("top"_string)));
    MUST(menus.create("two"_string, 1, item("top"_string)));
    // A background page that only idled out keeps its items, so creating one again fails as a duplicate.
    menus.extensions_changed({ { "one"_string, 1 }, { "two"_string, 1 } });
    EXPECT(menus.create("one"_string, 1, item("top"_string)).is_error());
    // A reload (a new revision) drops the extension's items, and so does its removal or disabling.
    menus.extensions_changed({ { "one"_string, 2 }, { "two"_string, 1 } });
    MUST(menus.create("one"_string, 2, item("top"_string)));
    menus.extensions_changed({ { "one"_string, 2 } });
    MUST(menus.create("two"_string, 1, item("top"_string)));
}

TEST_CASE(a_string_id_and_a_number_id_are_different_items)
{
    ExtensionMenus menus;
    MUST(menus.create("one"_string, 1, item("1"_string)));
    MUST(menus.create("one"_string, 1, item(static_cast<i64>(1))));
    MUST(menus.remove("one"_string, JsonValue { static_cast<i64>(1) }));
    // The string one is still there, and the number one is gone.
    EXPECT(menus.create("one"_string, 1, item("1"_string)).is_error());
    MUST(menus.create("one"_string, 1, item(static_cast<i64>(1))));
}

TEST_CASE(command_updates_go_with_a_removed_extension)
{
    WebView::ExtensionCommands commands;
    Vector<WebView::ExtensionManifest::Command> declared { { "toggle"_string, "Toggle"_string, "Alt+Shift+D"_string } };
    JsonObject change;
    change.set("name"sv, "toggle"_string);
    change.set("description"sv, "Changed"_string);
    MUST(commands.update("one"_string, declared, change));
    EXPECT_EQ(commands.commands("one"_string, declared)[0].description, "Changed"sv);
    commands.extensions_changed({ { "one"_string, 2 } });
    EXPECT_EQ(commands.commands("one"_string, declared)[0].description, "Changed"sv);
    commands.extensions_changed({});
    EXPECT_EQ(commands.commands("one"_string, declared)[0].description, "Toggle"sv);
}
