/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/ExtensionManifest.h>

namespace WebView {

// The context menu items each extension has created. Nothing shows them yet, so this only keeps what's needed to answer
// the calls as the API does: which ids exist, and which item each one's parent is. Extensions compare these errors'
// wording.
class WEBVIEW_API ExtensionMenus {
public:
    // The properties hold the item's id: the string the call gave, or the integer the renderer generated; a string and
    // an integer that read alike are different items. Items belong to the extension's revision that created them.
    ErrorOr<void, String> create(String const& extension_id, u64 revision, JsonObject properties);
    ErrorOr<void, String> update(String const& extension_id, JsonValue const& id, JsonObject const& properties);
    // Removes the item and every item under it.
    ErrorOr<void, String> remove(String const& extension_id, JsonValue const& id);
    void remove_all(String const& extension_id);
    // Drops the items of an extension that's gone, disabled, or reloaded (has a new revision); a background page that
    // only idled out keeps its items, as its extension stays loaded.
    void extensions_changed(HashMap<String, u64> const& revisions);

private:
    struct Item {
        String key;
        Optional<String> parent_key;
        JsonObject properties;
    };
    struct Items {
        u64 revision { 0 };
        Vector<Item> items;
    };
    HashMap<String, Items> m_items;
};

// What commands.getAll() reports: the manifest's commands, with what commands.update() changed since the browser
// started.
class WEBVIEW_API ExtensionCommands {
public:
    Vector<ExtensionManifest::Command> commands(String const& extension_id, Vector<ExtensionManifest::Command> const& declared) const;
    ErrorOr<void, String> update(String const& extension_id, Vector<ExtensionManifest::Command> const& declared, JsonObject const& details);
    // Forgets the changes of an extension that's no longer installed.
    void extensions_changed(HashMap<String, u64> const& revisions);

private:
    HashMap<String, HashMap<String, ExtensionManifest::Command>> m_updates;
};

}
