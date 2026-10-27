/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/ExtensionRegisteredScripts.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>

namespace WebView {

// A missing id counts as an empty one.
static ErrorOr<void, String> validate_id(StringView id)
{
    if (id.is_empty())
        return "Script's ID must not be empty"_string;
    if (id.starts_with('_'))
        return MUST(String::formatted("Script's ID '{}' must not start with '_'", id));
    return {};
}

static StringView id_of(JsonObject const& script)
{
    auto id = script.get_string("id"sv);
    return id.has_value() ? id->bytes_as_string_view() : StringView {};
}

static bool has_entries(JsonObject const& script, StringView key)
{
    auto array = script.get_array(key);
    return array.has_value() && !array->is_empty();
}

static ErrorOr<void, String> validate_script(JsonObject const& script)
{
    auto id = id_of(script);
    if (!script.get_array("matches"sv).has_value())
        return MUST(String::formatted("Script with ID '{}' must specify 'matches'.", id));
    if (!has_entries(script, "matches"sv))
        return MUST(String::formatted("Script with ID '{}' must specify at least one match.", id));
    if (!has_entries(script, "js"sv) && !has_entries(script, "css"sv))
        return MUST(String::formatted("Script with ID '{}' must specify at least one js or css file.", id));
    return {};
}

// A registered script reads back with every field it can have, at its default where the call left it out.
static JsonObject with_defaults(JsonObject script)
{
    auto set_default = [&](StringView key, JsonValue value) {
        if (!script.has(key))
            script.set(key, move(value));
    };
    set_default("allFrames"sv, false);
    set_default("matchOriginAsFallback"sv, false);
    set_default("runAt"sv, "document_idle"_string);
    set_default("world"sv, "ISOLATED"_string);
    set_default("persistAcrossSessions"sv, true);
    return script;
}

static bool persists(JsonObject const& script)
{
    return script.get_bool("persistAcrossSessions"sv).value_or(true);
}

static bool valid_extension_id(String const& id)
{
    return !id.is_empty() && id != "."sv && id != ".."sv && !id.contains('/') && !id.contains('\\') && !id.contains('\0');
}

ErrorOr<void> ExtensionRegisteredScripts::write_or_remove(ByteString const& path, Optional<String> const& contents)
{
    if (contents.has_value())
        return write_file_atomically(path, *contents);
    if (FileSystem::exists(path))
        TRY(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    return {};
}

ExtensionRegisteredScripts::ExtensionRegisteredScripts(Optional<ByteString> profile_data, Writer writer)
    : m_profile_data(move(profile_data))
    , m_writer(writer ? move(writer) : Writer { write_or_remove })
{
}

Optional<ByteString> ExtensionRegisteredScripts::path(String const& extension_id) const
{
    if (!m_profile_data.has_value() || !valid_extension_id(extension_id))
        return {};
    return LexicalPath::join(*m_profile_data, "extensions"sv, extension_id, "registered-content-scripts.json"sv).string();
}

Vector<JsonObject>& ExtensionRegisteredScripts::ensure_loaded(String const& extension_id)
{
    if (auto scripts = m_scripts.find(extension_id); scripts != m_scripts.end())
        return scripts->value;
    Vector<JsonObject> loaded;
    if (auto file_path = path(extension_id); file_path.has_value() && FileSystem::exists(*file_path)) {
        auto read = [&]() -> ErrorOr<JsonValue> {
            auto file = TRY(Core::File::open(*file_path, Core::File::OpenMode::Read));
            return JsonValue::from_string(TRY(file->read_until_eof()));
        };
        auto json = read();
        if (!json.is_error() && json.value().is_array()) {
            json.value().as_array().for_each([&](JsonValue const& script) {
                if (script.is_object() && script.as_object().get_string("id"sv).has_value())
                    loaded.append(script.as_object());
            });
        } else {
            warnln("Unable to read the registered content scripts of extension {}", extension_id);
        }
    }
    m_scripts.set(extension_id, move(loaded));
    return m_scripts.find(extension_id)->value;
}

ErrorOr<void, String> ExtensionRegisteredScripts::commit(String const& extension_id, Vector<JsonObject> scripts)
{
    if (auto file_path = path(extension_id); file_path.has_value()) {
        JsonArray kept;
        for (auto const& script : scripts) {
            if (persists(script))
                kept.must_append(script);
        }
        auto result = m_writer(*file_path, kept.is_empty() ? Optional<String> {} : Optional<String> { kept.serialized() });
        if (result.is_error())
            return MUST(String::formatted("Unable to save the registered content scripts: {}", result.error()));
    }
    m_scripts.set(extension_id, move(scripts));
    return {};
}

ErrorOr<void, String> ExtensionRegisteredScripts::register_scripts(String const& extension_id, Vector<JsonObject> scripts)
{
    auto& registered = ensure_loaded(extension_id);
    HashTable<String> ids;
    for (auto const& script : registered)
        ids.set(*script.get_string("id"sv));
    for (auto const& script : scripts) {
        auto id = id_of(script);
        TRY(validate_id(id));
        if (ids.set(MUST(String::from_utf8(id))) != HashSetResult::InsertedNewEntry)
            return MUST(String::formatted("Duplicate script ID '{}'", id));
        TRY(validate_script(script));
    }
    auto updated = registered;
    for (auto& script : scripts)
        updated.append(with_defaults(move(script)));
    return commit(extension_id, move(updated));
}

Vector<JsonObject> ExtensionRegisteredScripts::scripts(String const& extension_id, Optional<Vector<String>> const& ids)
{
    auto const& registered = ensure_loaded(extension_id);
    if (!ids.has_value())
        return registered;
    Vector<JsonObject> selected;
    for (auto const& script : registered) {
        if (ids->contains_slow(*script.get_string("id"sv)))
            selected.append(script);
    }
    return selected;
}

ErrorOr<void, String> ExtensionRegisteredScripts::update_scripts(String const& extension_id, Vector<JsonObject> changes)
{
    auto updated = ensure_loaded(extension_id);
    for (auto const& change : changes) {
        auto id = id_of(change);
        TRY(validate_id(id));
        auto script = updated.find_if([&](auto const& script) { return id_of(script) == id; });
        if (script == updated.end())
            return MUST(String::formatted("Nonexistent script ID '{}'", id));
        change.for_each_member([&](String const& key, JsonValue const& value) { script->set(key, value); });
        TRY(validate_script(*script));
    }
    return commit(extension_id, move(updated));
}

ErrorOr<void, String> ExtensionRegisteredScripts::unregister_scripts(String const& extension_id, Optional<Vector<String>> const& ids)
{
    auto remaining = ensure_loaded(extension_id);
    if (ids.has_value()) {
        for (auto const& id : *ids) {
            TRY(validate_id(id));
            if (!remaining.contains([&](auto const& script) { return script.get_string("id"sv) == id; }))
                return MUST(String::formatted("Nonexistent script ID '{}'", id));
        }
        remaining.remove_all_matching([&](auto const& script) { return ids->contains_slow(*script.get_string("id"sv)); });
    } else {
        remaining.clear();
    }
    return commit(extension_id, move(remaining));
}

void ExtensionRegisteredScripts::delete_scripts(String const& extension_id)
{
    if (auto result = commit(extension_id, {}); result.is_error()) {
        warnln("Unable to delete the registered content scripts of extension {}: {}", extension_id, result.error());
        m_scripts.set(extension_id, {});
    }
}

}
