/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionRegisteredScripts.h>

using WebView::ExtensionRegisteredScripts;

namespace {

struct Fixture {
    Fixture()
        : root(LexicalPath::join(Core::StandardPaths::tempfile_directory(), AK::generate_random_uuid()).string())
    {
        MUST(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes));
    }
    ~Fixture() { MUST(FileSystem::remove(root, FileSystem::RecursionMode::Allowed)); }
    ByteString root;
};

JsonObject script(StringView id, Optional<bool> persist = {})
{
    JsonObject object;
    object.set("id"sv, MUST(String::from_utf8(id)));
    JsonArray matches;
    matches.must_append("https://example.com/*"_string);
    object.set("matches"sv, move(matches));
    JsonArray js;
    js.must_append("cs.js"_string);
    object.set("js"sv, move(js));
    if (persist.has_value())
        object.set("persistAcrossSessions"sv, *persist);
    return object;
}

Vector<String> ids_of(Vector<JsonObject> const& scripts)
{
    Vector<String> ids;
    for (auto const& script : scripts)
        ids.append(*script.get_string("id"sv));
    return ids;
}

}

TEST_CASE(registered_scripts_persist_only_when_asked)
{
    Fixture f;
    {
        ExtensionRegisteredScripts scripts { f.root };
        MUST(scripts.register_scripts("one"_string, { script("kept"sv), script("gone"sv, false), script("also-kept"sv, true) }));
        EXPECT_EQ(ids_of(scripts.scripts("one"_string)), (Vector { "kept"_string, "gone"_string, "also-kept"_string }));
    }
    ExtensionRegisteredScripts restarted { f.root };
    EXPECT_EQ(ids_of(restarted.scripts("one"_string)), (Vector { "kept"_string, "also-kept"_string }));
    EXPECT(restarted.scripts("two"_string).is_empty());
}

TEST_CASE(a_duplicate_id_rejects_and_nothing_registers)
{
    ExtensionRegisteredScripts scripts { {} };
    MUST(scripts.register_scripts("one"_string, { script("a"sv) }));
    auto result = scripts.register_scripts("one"_string, { script("b"sv), script("a"sv) });
    EXPECT(result.is_error());
    EXPECT_EQ(result.error(), "Duplicate script ID 'a'"sv);
    EXPECT_EQ(ids_of(scripts.scripts("one"_string)), (Vector { "a"_string }));

    auto twice = scripts.register_scripts("one"_string, { script("c"sv), script("c"sv) });
    EXPECT_EQ(twice.error(), "Duplicate script ID 'c'"sv);
    EXPECT_EQ(ids_of(scripts.scripts("one"_string)), (Vector { "a"_string }));
}

TEST_CASE(invalid_scripts_reject)
{
    ExtensionRegisteredScripts scripts { {} };
    EXPECT_EQ(scripts.register_scripts("one"_string, { script(""sv) }).error(), "Script's ID must not be empty"sv);
    EXPECT_EQ(scripts.register_scripts("one"_string, { script("_hidden"sv) }).error(), "Script's ID '_hidden' must not start with '_'"sv);
    auto no_matches = script("a"sv);
    no_matches.remove("matches"sv);
    EXPECT_EQ(scripts.register_scripts("one"_string, { no_matches }).error(), "Script with ID 'a' must specify 'matches'."sv);
    auto no_files = script("a"sv);
    no_files.remove("js"sv);
    EXPECT_EQ(scripts.register_scripts("one"_string, { no_files }).error(), "Script with ID 'a' must specify at least one js or css file."sv);
    EXPECT(scripts.scripts("one"_string).is_empty());
}

TEST_CASE(update_changes_only_the_given_fields)
{
    ExtensionRegisteredScripts scripts { {} };
    auto original = script("a"sv);
    original.set("runAt"sv, "document_start"_string);
    MUST(scripts.register_scripts("one"_string, { original }));

    JsonObject change;
    change.set("id"sv, "a"_string);
    change.set("allFrames"sv, true);
    MUST(scripts.update_scripts("one"_string, { change }));
    auto updated = scripts.scripts("one"_string).first();
    EXPECT_EQ(updated.get_bool("allFrames"sv), true);
    EXPECT_EQ(updated.get_string("runAt"sv), "document_start"_string);
    EXPECT_EQ(updated.get_array("js"sv)->size(), 1u);

    JsonObject missing;
    missing.set("id"sv, "nope"_string);
    EXPECT_EQ(scripts.update_scripts("one"_string, { missing }).error(), "Nonexistent script ID 'nope'"sv);
}

TEST_CASE(unregister_with_no_ids_removes_all)
{
    ExtensionRegisteredScripts scripts { {} };
    MUST(scripts.register_scripts("one"_string, { script("a"sv), script("b"sv), script("c"sv) }));
    MUST(scripts.register_scripts("two"_string, { script("a"sv) }));
    EXPECT_EQ(scripts.unregister_scripts("one"_string, Vector { "nope"_string }).error(), "Nonexistent script ID 'nope'"sv);
    MUST(scripts.unregister_scripts("one"_string, Vector { "b"_string }));
    EXPECT_EQ(ids_of(scripts.scripts("one"_string)), (Vector { "a"_string, "c"_string }));
    EXPECT_EQ(ids_of(scripts.scripts("one"_string, Vector { "c"_string })), (Vector { "c"_string }));
    MUST(scripts.unregister_scripts("one"_string, {}));
    EXPECT(scripts.scripts("one"_string).is_empty());
    EXPECT_EQ(scripts.scripts("two"_string).size(), 1u);
}

TEST_CASE(deleting_an_extension_removes_its_file)
{
    Fixture f;
    {
        ExtensionRegisteredScripts scripts { f.root };
        MUST(scripts.register_scripts("one"_string, { script("a"sv) }));
        scripts.delete_scripts("one"_string);
        EXPECT(scripts.scripts("one"_string).is_empty());
    }
    ExtensionRegisteredScripts restarted { f.root };
    EXPECT(restarted.scripts("one"_string).is_empty());
}

TEST_CASE(a_failed_save_fails_the_change_and_keeps_the_old_scripts)
{
    Fixture f;
    bool fail = false;
    ExtensionRegisteredScripts::Writer writer = [&](ByteString const& path, Optional<String> const& contents) -> ErrorOr<void> {
        if (fail)
            return Error::from_string_literal("disk full");
        return ExtensionRegisteredScripts::write_or_remove(path, contents);
    };
    {
        ExtensionRegisteredScripts scripts { f.root, move(writer) };
        MUST(scripts.register_scripts("one"_string, { script("a"sv) }));
        fail = true;
        EXPECT(scripts.register_scripts("one"_string, { script("b"sv) }).is_error());
        EXPECT(scripts.unregister_scripts("one"_string, {}).is_error());
        JsonObject change;
        change.set("id"sv, "a"_string);
        change.set("persistAcrossSessions"sv, false);
        EXPECT(scripts.update_scripts("one"_string, { change }).is_error());
        EXPECT_EQ(ids_of(scripts.scripts("one"_string)), (Vector { "a"_string }));
        EXPECT_EQ(scripts.scripts("one"_string).first().get_bool("persistAcrossSessions"sv), true);
    }
    ExtensionRegisteredScripts restarted { f.root };
    EXPECT_EQ(ids_of(restarted.scripts("one"_string)), (Vector { "a"_string }));
}
