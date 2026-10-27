/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// The content scripts each extension has registered with scripting.registerContentScripts(), as the API describes
// them. Those with persistAcrossSessions (the default) are kept in a file of the extension's own, and come back after a
// restart; the rest last until then. Extensions compare these errors' wording.
class WEBVIEW_API ExtensionRegisteredScripts {
    AK_ALLOC_WITH_KMALLOC;

public:
    // Writes the file at the path, or with no contents, removes it.
    using Writer = Function<ErrorOr<void>(ByteString const& path, Optional<String> const& contents)>;
    static ErrorOr<void> write_or_remove(ByteString const& path, Optional<String> const& contents);

    // With no profile data directory, nothing is kept across a restart. A change that can't be saved fails, and leaves
    // the scripts as they were.
    explicit ExtensionRegisteredScripts(Optional<ByteString> profile_data, Writer = {});

    // Registers every script, with each field the call leaves out at its default, or none of them.
    ErrorOr<void, String> register_scripts(String const& extension_id, Vector<JsonObject> scripts);
    // The scripts with the given ids, or all of them, in the order they were registered.
    Vector<JsonObject> scripts(String const& extension_id, Optional<Vector<String>> const& ids = {});
    // Changes the given fields of each named script, or none of them.
    ErrorOr<void, String> update_scripts(String const& extension_id, Vector<JsonObject> changes);
    // Unregisters the scripts with the given ids, or all of them.
    ErrorOr<void, String> unregister_scripts(String const& extension_id, Optional<Vector<String>> const& ids);
    // Forgets every script of a removed extension, and its file.
    void delete_scripts(String const& extension_id);

private:
    Vector<JsonObject>& ensure_loaded(String const& extension_id);
    // Saves the scripts as they would be after a change, and only then makes the change.
    ErrorOr<void, String> commit(String const& extension_id, Vector<JsonObject> scripts);
    Optional<ByteString> path(String const& extension_id) const;

    Optional<ByteString> m_profile_data;
    Writer m_writer;
    HashMap<String, Vector<JsonObject>> m_scripts;
};

}
