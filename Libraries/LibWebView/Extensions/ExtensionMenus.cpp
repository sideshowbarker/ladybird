/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionMenus.h>

namespace WebView {

// An id reads the same in an error whichever kind it is ("top" for a string, "1" for a generated integer), but is kept
// with its kind, so a string "1" and the integer 1 are different items.
static Optional<String> id_text(JsonValue const& id)
{
    if (id.is_string())
        return id.as_string();
    if (auto number = id.get_integer<i64>(); number.has_value())
        return String::number(*number);
    return {};
}

static Optional<String> id_key(JsonValue const& id)
{
    auto text = id_text(id);
    if (!text.has_value())
        return {};
    return MUST(String::formatted("{}{}", id.is_string() ? "s:"sv : "n:"sv, *text));
}

static String not_found(JsonValue const& id)
{
    return MUST(String::formatted("Cannot find menu item with id {}", id_text(id).value_or("undefined"_string)));
}

ErrorOr<void, String> ExtensionMenus::create(String const& extension_id, u64 revision, JsonObject properties)
{
    auto id = properties.get("id"sv).value_or({});
    auto key = id_key(id);
    if (!key.has_value())
        return "A menu item needs an id"_string;
    auto& entry = m_items.ensure(extension_id);
    // The items of an earlier load of the extension don't outlive it.
    if (entry.revision != revision) {
        entry.revision = revision;
        entry.items.clear();
    }
    auto& items = entry.items;
    if (items.contains([&](auto const& item) { return item.key == *key; }))
        return MUST(String::formatted("Cannot create item with duplicate id {}", *id_text(id)));
    Optional<String> parent_key;
    if (auto parent = properties.get("parentId"sv); parent.has_value()) {
        parent_key = id_key(*parent);
        if (!parent_key.has_value() || !items.contains([&](auto const& item) { return item.key == *parent_key; }))
            return not_found(*parent);
    }
    items.append({ key.release_value(), move(parent_key), move(properties) });
    return {};
}

ErrorOr<void, String> ExtensionMenus::update(String const& extension_id, JsonValue const& id, JsonObject const& properties)
{
    auto key = id_key(id);
    auto& items = m_items.ensure(extension_id).items;
    auto item = items.find_if([&](auto const& item) { return key.has_value() && item.key == *key; });
    if (item == items.end())
        return not_found(id);
    if (auto parent = properties.get("parentId"sv); parent.has_value()) {
        auto parent_key = id_key(*parent);
        if (!parent_key.has_value() || !items.contains([&](auto const& other) { return other.key == *parent_key; }))
            return not_found(*parent);
        item->parent_key = parent_key;
    }
    properties.for_each_member([&](String const& name, JsonValue const& value) { item->properties.set(name, value); });
    return {};
}

ErrorOr<void, String> ExtensionMenus::remove(String const& extension_id, JsonValue const& id)
{
    auto key = id_key(id);
    auto& items = m_items.ensure(extension_id).items;
    if (!key.has_value() || !items.contains([&](auto const& item) { return item.key == *key; }))
        return not_found(id);
    HashTable<String> removed;
    removed.set(*key);
    // Children name their parents, so each pass takes away the items under one already taken away.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto const& item : items) {
            if (item.parent_key.has_value() && removed.contains(*item.parent_key) && removed.set(item.key) == HashSetResult::InsertedNewEntry)
                changed = true;
        }
    }
    items.remove_all_matching([&](auto const& item) { return removed.contains(item.key); });
    return {};
}

void ExtensionMenus::remove_all(String const& extension_id)
{
    m_items.ensure(extension_id).items.clear();
}

void ExtensionMenus::extensions_changed(HashMap<String, u64> const& revisions)
{
    m_items.remove_all_matching([&](auto const& extension_id, auto const& entry) { return revisions.get(extension_id) != entry.revision; });
}

void ExtensionCommands::extensions_changed(HashMap<String, u64> const& revisions)
{
    m_updates.remove_all_matching([&](auto const& extension_id, auto const&) { return !revisions.contains(extension_id); });
}

Vector<ExtensionManifest::Command> ExtensionCommands::commands(String const& extension_id, Vector<ExtensionManifest::Command> const& declared) const
{
    auto commands = declared;
    if (auto updates = m_updates.get(extension_id); updates.has_value()) {
        for (auto& command : commands) {
            if (auto updated = updates->get(command.name); updated.has_value())
                command = *updated;
        }
    }
    return commands;
}

ErrorOr<void, String> ExtensionCommands::update(String const& extension_id, Vector<ExtensionManifest::Command> const& declared, JsonObject const& details)
{
    auto name = details.get_string("name"sv).value_or({});
    auto current = commands(extension_id, declared);
    auto command = current.find_if([&](auto const& command) { return command.name == name; });
    if (command == current.end())
        return MUST(String::formatted("Unknown command \"{}\"", name));
    auto updated = *command;
    if (auto description = details.get_string("description"sv); description.has_value())
        updated.description = *description;
    if (auto shortcut = details.get_string("shortcut"sv); shortcut.has_value())
        updated.shortcut = shortcut->is_empty() ? Optional<String> {} : Optional<String> { *shortcut };
    m_updates.ensure(extension_id).set(name, move(updated));
    return {};
}

}
