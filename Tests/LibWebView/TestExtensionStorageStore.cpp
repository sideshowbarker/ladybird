/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>

using WebView::ExtensionStorageStore;
using WebView::StorageChanges;

namespace {

struct Fixture {
    Fixture()
        : root(LexicalPath::join(Core::StandardPaths::tempfile_directory(), AK::generate_random_uuid()).string())
    {
        MUST(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes));
    }
    ~Fixture() { MUST(FileSystem::remove(root, FileSystem::RecursionMode::Allowed)); }
    ByteString path() const { return LexicalPath::join(root, "extensions/one/storage-local.json"sv).string(); }
    void write(StringView text)
    {
        MUST(Core::Directory::create(LexicalPath(path()).dirname(), Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(path(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(text.bytes()));
    }
    String read() const
    {
        auto file = MUST(Core::File::open(path(), Core::File::OpenMode::Read));
        return MUST(String::from_utf8(MUST(file->read_until_eof())));
    }
    Core::EventLoop loop;
    ByteString root;
};

template<typename T, typename Start>
ErrorOr<T> wait_for(Core::EventLoop& loop, Start start)
{
    Optional<ErrorOr<T>> result;
    start([&](ErrorOr<T> value) { result = move(value); });
    loop.spin_until([&] { return result.has_value(); });
    return result.release_value();
}

ErrorOr<StorageChanges> set(Fixture& f, ExtensionStorageStore& store, JsonObject items, bool unlimited = false)
{
    return wait_for<StorageChanges>(f.loop, [&](auto callback) { store.set("one"_string, move(items), unlimited, move(callback)); });
}
JsonObject get(Fixture& f, ExtensionStorageStore& store)
{
    return MUST(wait_for<JsonObject>(f.loop, [&](auto callback) { store.get("one"_string, {}, move(callback)); }));
}
u64 bytes(Fixture& f, ExtensionStorageStore& store)
{
    return MUST(wait_for<u64>(f.loop, [&](auto callback) { store.bytes_in_use("one"_string, {}, move(callback)); }));
}

}

TEST_CASE(store_can_be_created_before_the_event_loop)
{
    auto root = LexicalPath::join(Core::StandardPaths::tempfile_directory(), AK::generate_random_uuid()).string();
    ExtensionStorageStore store(root);
    Core::EventLoop loop;
    auto items = TRY_OR_FAIL(wait_for<JsonObject>(loop, [&](auto callback) { store.get("one"_string, {}, move(callback)); }));
    EXPECT(items.is_empty());
}

TEST_CASE(quota_boundary_is_exact)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    auto key = MUST(String::from_utf8("\xc3\xa9"sv));
    JsonObject items;
    items.set(key, MUST(String::repeated('a', ExtensionStorageStore::quota_bytes - 4)));
    EXPECT(!set(f, store, items).is_error());
    EXPECT_EQ(bytes(f, store), ExtensionStorageStore::quota_bytes);
    auto before = f.read();
    items.set(key, MUST(String::repeated('a', ExtensionStorageStore::quota_bytes - 3)));
    EXPECT(set(f, store, items).is_error());
    EXPECT_EQ(bytes(f, store), ExtensionStorageStore::quota_bytes);
    EXPECT(f.read() == before);
    EXPECT(!set(f, store, move(items), true).is_error());
    EXPECT_EQ(bytes(f, store), ExtensionStorageStore::quota_bytes + 1);
}

TEST_CASE(set_is_atomic_and_unchanged_values_have_no_changes)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    JsonObject initial;
    initial.set("a"sv, 1);
    auto changes = TRY_OR_FAIL(set(f, store, initial));
    EXPECT(!changes.get("a"_string)->old_value.has_value());
    EXPECT(changes.get("a"_string)->new_value.has_value());
    EXPECT(TRY_OR_FAIL(set(f, store, initial)).is_empty());
    JsonObject too_large;
    too_large.set("a"sv, 2);
    too_large.set("large"sv, MUST(String::repeated('a', ExtensionStorageStore::quota_bytes)));
    EXPECT(set(f, store, move(too_large)).is_error());
    EXPECT_EQ(get(f, store).serialized(), initial.serialized());
}

TEST_CASE(operations_apply_in_request_order)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    size_t completed = 0;
    for (int value = 1; value <= 3; ++value) {
        JsonObject items;
        items.set("key"sv, value);
        store.set("one"_string, move(items), false, [&, value](ErrorOr<StorageChanges> result) {
            EXPECT(!result.is_error());
            if (result.is_error()) {
                ++completed;
                return;
            }
            auto change = result.value().get("key"_string);
            EXPECT(change.has_value());
            if (change.has_value()) {
                EXPECT_EQ(change->new_value->as_integer<i32>(), value);
                if (value == 1)
                    EXPECT(!change->old_value.has_value());
                else
                    EXPECT_EQ(change->old_value->as_integer<i32>(), value - 1);
            }
            EXPECT_EQ(++completed, static_cast<size_t>(value));
        });
    }
    f.loop.spin_until([&] { return completed == 3; });
    EXPECT_EQ(get(f, store).get_i32("key"sv).value(), 3);
}

TEST_CASE(remove_clear_and_reopen)
{
    Fixture f;
    {
        ExtensionStorageStore store(f.root);
        JsonObject items;
        items.set("one"sv, 1);
        items.set("two"sv, JsonValue {});
        EXPECT(!set(f, store, move(items)).is_error());
    }
    ExtensionStorageStore reopened(f.root);
    EXPECT_EQ(get(f, reopened).size(), 2u);
    auto changes = TRY_OR_FAIL(wait_for<StorageChanges>(f.loop, [&](auto callback) { reopened.remove("one"_string, { "one"_string, "missing"_string }, move(callback)); }));
    EXPECT_EQ(changes.size(), 1u);
    EXPECT(changes.get("one"_string)->old_value.has_value());
    EXPECT(!changes.get("one"_string)->new_value.has_value());
    changes = TRY_OR_FAIL(wait_for<StorageChanges>(f.loop, [&](auto callback) { reopened.clear("one"_string, move(callback)); }));
    EXPECT_EQ(changes.size(), 1u);
    EXPECT(changes.get("two"_string)->old_value->is_null());
    EXPECT(!changes.get("two"_string)->new_value.has_value());
    EXPECT(get(f, reopened).is_empty());
}

TEST_CASE(write_failure_keeps_old_map)
{
    Fixture f;
    f.write(R"({"formatVersion":1,"items":{"key":1}})"sv);
    auto before = f.read();
    ExtensionStorageStore store(f.root, [](ByteString const&, String const&) -> ErrorOr<void> { return Error::from_errno(EIO); });
    JsonObject items;
    items.set("key"sv, 2);
    EXPECT(set(f, store, move(items)).is_error());
    EXPECT_EQ(get(f, store).get_i32("key"sv).value(), 1);
    EXPECT(f.read() == before);
}

TEST_CASE(corrupt_file_is_left_alone)
{
    for (auto content : { "not JSON"sv, R"({"formatVersion":2,"items":{}})"sv }) {
        Fixture f;
        f.write(content);
        ExtensionStorageStore store(f.root);
        auto result = wait_for<JsonObject>(f.loop, [&](auto callback) { store.get("one"_string, {}, move(callback)); });
        EXPECT(result.is_error());
        if (result.is_error())
            EXPECT(result.error().string_literal().contains("storage-local.json"sv));
        JsonObject items;
        items.set("key"sv, 1);
        EXPECT(set(f, store, move(items)).is_error());
        EXPECT(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.remove("one"_string, { "key"_string }, move(callback)); }).is_error());
        EXPECT(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.clear("one"_string, move(callback)); }).is_error());
        EXPECT(wait_for<u64>(f.loop, [&](auto callback) { store.bytes_in_use("one"_string, {}, move(callback)); }).is_error());
        EXPECT_EQ(f.read(), content);
    }
}

TEST_CASE(delete_storage_removes_directory_and_resets_cache)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    JsonObject items;
    items.set("key"sv, 1);
    EXPECT(!set(f, store, move(items)).is_error());
    EXPECT(!wait_for<void>(f.loop, [&](auto callback) { store.delete_storage("one"_string, move(callback)); }).is_error());
    EXPECT(!FileSystem::exists(LexicalPath(f.path()).dirname()));
    EXPECT(get(f, store).is_empty());
}

TEST_CASE(nesting_limit_accounts_for_persistence_and_event_envelopes)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    auto nested = [](size_t depth) {
        StringBuilder builder;
        builder.append(R"({"deep":)"sv);
        builder.append_repeated('[', depth);
        builder.append('0');
        builder.append_repeated(']', depth);
        builder.append('}');
        return MUST(JsonValue::from_string(builder.string_view())).as_object();
    };
    EXPECT(!set(f, store, nested(510)).is_error());
    auto before = f.read();
    EXPECT(set(f, store, nested(511)).is_error());
    EXPECT(f.read() == before);
    ExtensionStorageStore reopened(f.root);
    auto loaded = TRY_OR_FAIL(wait_for<JsonObject>(f.loop, [&](auto callback) { reopened.get("one"_string, {}, move(callback)); }));
    EXPECT(loaded.has("deep"sv));
}

TEST_CASE(quota_counts_json_escapes_and_number_representations)
{
    Fixture f;
    ExtensionStorageStore store(f.root);
    struct Case {
        JsonValue value;
        u64 bytes;
    };
    for (auto const& example : { Case { "\r"sv, 5 }, Case { "\f"sv, 5 }, Case { "<"sv, 9 }, Case { "\xe2\x80\xa8"sv, 9 }, Case { "\xe2\x80\xa9"sv, 9 }, Case { JsonValue(2147483648LL), 13 }, Case { JsonValue(1e12), 6 }, Case { JsonValue(1234567890123.5), 20 }, Case { JsonValue(1e-7), 5 }, Case { JsonValue(1e-6), 9 }, Case { MUST(JsonValue::from_string(R"({"<":"\r"})"sv)), 16 } }) {
        JsonObject items;
        items.set("k"sv, example.value);
        EXPECT(!set(f, store, move(items)).is_error());
        EXPECT_EQ(bytes(f, store), example.bytes);
    }
    JsonObject items;
    items.set("k"sv, MUST(String::formatted("<{}", MUST(String::repeated('a', ExtensionStorageStore::quota_bytes - 9)))));
    EXPECT(!set(f, store, items).is_error());
    EXPECT_EQ(bytes(f, store), ExtensionStorageStore::quota_bytes);
    auto before = f.read();
    items.set("k"sv, MUST(String::formatted("<{}", MUST(String::repeated('a', ExtensionStorageStore::quota_bytes - 8)))));
    EXPECT(set(f, store, move(items)).is_error());
    EXPECT(f.read() == before);
}

TEST_CASE(quota_total_stays_exact_across_mutations_and_reopen)
{
    Fixture f;
    {
        ExtensionStorageStore store(f.root);
        JsonObject items;
        items.set("a"sv, "<"sv);
        items.set("b"sv, 1);
        TRY_OR_FAIL(set(f, store, items));
        EXPECT_EQ(bytes(f, store), 11u);
        EXPECT(TRY_OR_FAIL(set(f, store, items)).is_empty());
        EXPECT_EQ(bytes(f, store), 11u);

        JsonObject replacement;
        replacement.set("a"sv, 12);
        replacement.set("c"sv, "\r"sv);
        auto changes = TRY_OR_FAIL(set(f, store, move(replacement)));
        EXPECT_EQ(changes.size(), 2u);
        EXPECT_EQ(bytes(f, store), 10u);
        auto selected = TRY_OR_FAIL(wait_for<u64>(f.loop, [&](auto callback) { store.bytes_in_use("one"_string, Vector { "a"_string, "c"_string, "a"_string, "missing"_string }, move(callback)); }));
        EXPECT_EQ(selected, 8u);

        changes = TRY_OR_FAIL(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.remove("one"_string, { "a"_string, "a"_string, "missing"_string }, move(callback)); }));
        EXPECT_EQ(changes.size(), 1u);
        EXPECT_EQ(bytes(f, store), 7u);
    }
    {
        ExtensionStorageStore store(f.root, [](ByteString const&, String const&) -> ErrorOr<void> { return Error::from_errno(EIO); });
        EXPECT_EQ(bytes(f, store), 7u);
        JsonObject items;
        items.set("b"sv, "larger"sv);
        EXPECT(set(f, store, move(items)).is_error());
        EXPECT_EQ(bytes(f, store), 7u);
        EXPECT(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.remove("one"_string, { "b"_string }, move(callback)); }).is_error());
        EXPECT_EQ(bytes(f, store), 7u);
        EXPECT(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.clear("one"_string, move(callback)); }).is_error());
        EXPECT_EQ(bytes(f, store), 7u);
    }
    {
        ExtensionStorageStore store(f.root);
        EXPECT_EQ(bytes(f, store), 7u);
        auto changes = TRY_OR_FAIL(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.clear("one"_string, move(callback)); }));
        EXPECT_EQ(changes.size(), 2u);
        EXPECT_EQ(bytes(f, store), 0u);
        EXPECT(TRY_OR_FAIL(wait_for<StorageChanges>(f.loop, [&](auto callback) { store.clear("one"_string, move(callback)); })).is_empty());
        JsonObject items;
        items.set("a"sv, 1);
        TRY_OR_FAIL(set(f, store, move(items)));
        EXPECT_EQ(bytes(f, store), 2u);
        TRY_OR_FAIL(wait_for<void>(f.loop, [&](auto callback) { store.delete_storage("one"_string, move(callback)); }));
        EXPECT_EQ(bytes(f, store), 0u);
    }
    ExtensionStorageStore reopened(f.root);
    EXPECT_EQ(bytes(f, reopened), 0u);
}
