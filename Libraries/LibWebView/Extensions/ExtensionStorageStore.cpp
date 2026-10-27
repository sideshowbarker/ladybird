/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <AK/Mutex.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf8View.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibThreading/Thread.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#    include <io.h>
#else
#    include <unistd.h>
#endif

namespace WebView {

static bool valid_id(String const& id)
{
    return !id.is_empty() && id != "."sv && id != ".."sv && !id.contains('/') && !id.contains('\\') && !id.contains('\0');
}

ErrorOr<void> write_file_atomically(ByteString const& path, String const& contents)
{
    TRY(Core::Directory::create(LexicalPath(path).dirname(), Core::Directory::CreateDirectories::Yes, 0700));
    auto temporary = ByteString::formatted("{}.{}", path, AK::generate_random_uuid());
    auto file = TRY(Core::File::open(temporary, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew, 0600));
    ArmedScopeGuard remove_temporary = [&] { (void)FileSystem::remove(temporary, FileSystem::RecursionMode::Disallowed); };
    TRY(file->write_until_depleted(contents.bytes()));
#ifdef AK_OS_WINDOWS
    if (_commit(file->fd()) < 0)
        return Error::from_errno(errno);
    file->close();
    auto destination = TRY(to_wide_string(path));
    auto source = TRY(to_wide_string(temporary));
    if (FileSystem::exists(path)) {
        if (!ReplaceFileW(destination.data(), source.data(), nullptr, 0, nullptr, nullptr))
            return Error::from_windows_error();
    } else if (!MoveFileExW(source.data(), destination.data(), MOVEFILE_WRITE_THROUGH)) {
        return Error::from_windows_error();
    }
#else
    if (fsync(file->fd()) < 0)
        return Error::from_errno(errno);
    file->close();
    TRY(Core::System::rename(temporary, path));
#endif
    remove_temporary.disarm();
    return {};
}

static u64 quoted_json_string_size(String const& string)
{
    u64 size = 2;
    for (auto code_point : Utf8View(string)) {
        if (code_point == '\b' || code_point == '\f' || code_point == '\n' || code_point == '\r' || code_point == '\t' || code_point == '\\' || code_point == '"')
            size += 2;
        else if (code_point < 0x20 || code_point == '<' || code_point == 0x2028 || code_point == 0x2029)
            size += 6;
        else
            size += code_point < 0x80 ? 1 : code_point < 0x800 ? 2
                : code_point < 0x10000                         ? 3
                                                               : 4;
    }
    return size;
}

static u64 quota_json_size(JsonValue const& value)
{
    if (value.is_string())
        return quoted_json_string_size(value.as_string());
    if (value.is_array()) {
        u64 size = 2 + (value.as_array().size() ? value.as_array().size() - 1 : 0);
        for (auto const& item : value.as_array().values())
            size += quota_json_size(item);
        return size;
    }
    if (value.is_object()) {
        u64 size = 2 + (value.as_object().size() ? value.as_object().size() - 1 : 0);
        value.as_object().for_each_member([&](auto const& key, auto const& item) { size += quoted_json_string_size(key) + 1 + quota_json_size(item); });
        return size;
    }
    if (value.is_number()) {
        auto number = value.get_double_with_precision_loss().value();
        if (number >= NumericLimits<i32>::min() && number <= NumericLimits<i32>::max() && number == static_cast<i32>(number))
            return MUST(String::formatted("{}", static_cast<i32>(number))).byte_count();
        auto serialized = MUST(String::formatted("{}", number));
        auto text = serialized.bytes_as_string_view();
        auto magnitude = number < 0 ? -number : number;
        // Quota JSON uses scientific notation from 1e12 and preserves the type of non-32-bit integers with ".0".
        if (magnitude >= 1e12 && magnitude < 1e21) {
            auto unsigned_text = text.trim("-"sv, TrimMode::Left);
            auto point = unsigned_text.find('.');
            auto exponent = point.value_or(unsigned_text.length()) - 1;
            auto digits = unsigned_text.trim("0"sv, TrimMode::Right).length() - (point.has_value() ? 1 : 0);
            return (number < 0 ? 1 : 0) + digits + (digits > 1 ? 1 : 0) + 2 + MUST(String::formatted("{}", exponent)).byte_count();
        }
        return text.length() + (!text.contains('.') && !text.contains('e') ? 2 : 0);
    }
    return value.serialized().byte_count();
}

static ErrorOr<void> check_nesting_depth(JsonValue const& value, size_t remaining_depth)
{
    if (!value.is_array() && !value.is_object())
        return {};
    if (remaining_depth == 0)
        return Error::from_string_literal("Storage values exceed the supported JSON nesting depth");
    if (value.is_array()) {
        for (auto const& item : value.as_array().values())
            TRY(check_nesting_depth(item, remaining_depth - 1));
    } else {
        TRY(value.as_object().try_for_each_member([&](auto const&, auto const& item) { return check_nesting_depth(item, remaining_depth - 1); }));
    }
    return {};
}

struct ExtensionStorageStore::Impl {
    AK_ALLOC_WITH_KMALLOC;

    Impl(ByteString root, Writer writer)
        : root(move(root))
        , writer(writer ? move(writer) : Writer { write_file_atomically })
        , worker(Threading::Thread::construct("Extension storage"sv, [this] { return run(); }))
    {
    }

    ~Impl()
    {
        if (!worker->is_started())
            return;
        {
            MutexLocker locker(mutex);
            stopping = true;
            condition.signal();
        }
        MUST(worker->join());
    }

    template<typename T>
    void enqueue(Function<ErrorOr<T>()> action, Function<void(ErrorOr<T>)> callback)
    {
        if (!loop)
            loop = &Core::EventLoop::current();
        {
            MutexLocker locker(mutex);
            actions.append([this, action = move(action), callback = move(callback)]() mutable {
                auto result = action();
                loop->deferred_invoke([callback = move(callback), result = move(result)]() mutable { callback(move(result)); });
            });
            condition.signal();
        }
        if (!worker->is_started())
            worker->start();
    }

    intptr_t run()
    {
        while (true) {
            Function<void()> action;
            {
                MutexLocker locker(mutex);
                condition.wait_while([&] { return actions.is_empty() && !stopping; });
                if (actions.is_empty())
                    return 0;
                action = actions.take_first();
            }
            action();
        }
    }

    ByteString directory(String const& id) const { return LexicalPath::join(root, "extensions"sv, id).string(); }
    ByteString path(String const& id) const { return LexicalPath::join(directory(id), is_sync ? "storage-sync.json"sv : "storage-local.json"sv).string(); }

    struct Entry {
        Optional<JsonObject> items;
        u64 bytes_in_use { 0 };
        bool invalid { false };
    };

    ErrorOr<Entry*> load(String const& id)
    {
        if (!valid_id(id))
            return Error::from_string_literal("Invalid extension storage id");
        auto& entry = entries.ensure(id);
        if (entry.invalid)
            return is_sync ? Error::from_string_literal("storage-sync.json is corrupt or has an unsupported formatVersion") : Error::from_string_literal("storage-local.json is corrupt or has an unsupported formatVersion");
        if (entry.items.has_value())
            return &entry;
        if (in_memory) {
            entry.items = JsonObject {};
            return &entry;
        }
        auto file = Core::File::open(path(id), Core::File::OpenMode::Read);
        if (file.is_error()) {
            if (!file.error().is_errno() || file.error().code() != ENOENT)
                return file.release_error();
            entry.items = JsonObject {};
            return &entry;
        }
        auto data = TRY(file.value()->read_until_eof());
        auto parsed = JsonValue::from_string(data);
        if (parsed.is_error() || !parsed.value().is_object() || parsed.value().as_object().get_u32("formatVersion"sv) != 1u || !parsed.value().as_object().has_object("items"sv)) {
            entry.invalid = true;
            return is_sync ? Error::from_string_literal("storage-sync.json is corrupt or has an unsupported formatVersion") : Error::from_string_literal("storage-local.json is corrupt or has an unsupported formatVersion");
        }
        entry.items = *parsed.value().as_object().get_object("items"sv);
        entry.bytes_in_use = size(*entry.items);
        return &entry;
    }

    static JsonObject select(JsonObject const& items, Optional<Vector<String>> const& keys)
    {
        if (!keys.has_value())
            return items;
        JsonObject result;
        for (auto const& key : *keys) {
            if (auto value = items.get(key); value.has_value())
                result.set(key, *value);
        }
        return result;
    }

    static u64 size(JsonObject const& items)
    {
        u64 total = 0;
        items.for_each_member([&](auto const& key, auto const& value) {
            total += key.bytes().size() + quota_json_size(value);
        });
        return total;
    }

    ErrorOr<StorageChanges> mutate(String const& id, Function<ErrorOr<StorageChanges>(JsonObject const&)> change, bool unlimited)
    {
        auto* entry = TRY(load(id));
        auto changes = TRY(change(*entry->items));
        auto bytes_in_use = entry->bytes_in_use;
        for (auto const& [key, values] : changes) {
            if (values.old_value.has_value())
                bytes_in_use -= key.byte_count() + quota_json_size(*values.old_value);
            if (values.new_value.has_value())
                bytes_in_use += key.byte_count() + quota_json_size(*values.new_value);
        }
        if (in_memory && bytes_in_use > ExtensionStorageStore::quota_bytes)
            return Error::from_string_literal("storage.session QUOTA_BYTES quota exceeded");
        // Extensions compare these errors' wording.
        if (is_sync) {
            for (auto const& [key, values] : changes) {
                if (values.new_value.has_value() && key.byte_count() + quota_json_size(*values.new_value) > ExtensionStorageStore::sync_quota_bytes_per_item)
                    return Error::from_string_literal("QUOTA_BYTES_PER_ITEM quota exceeded");
            }
            auto items = entry->items->size();
            for (auto const& [key, values] : changes) {
                if (values.new_value.has_value() && !entry->items->has(key))
                    ++items;
                else if (!values.new_value.has_value() && entry->items->has(key))
                    --items;
            }
            if (items > ExtensionStorageStore::sync_max_items)
                return Error::from_string_literal("MAX_ITEMS quota exceeded");
            if (bytes_in_use > ExtensionStorageStore::sync_quota_bytes)
                return Error::from_string_literal("QUOTA_BYTES quota exceeded");
        }
        if (!is_sync && !unlimited && bytes_in_use > ExtensionStorageStore::quota_bytes)
            return Error::from_string_literal("storage.local QUOTA_BYTES quota exceeded");
        if (changes.is_empty())
            return changes;
        auto candidate = *entry->items;
        for (auto const& [key, values] : changes) {
            if (values.new_value.has_value())
                candidate.set(key, *values.new_value);
            else
                candidate.remove(key);
        }
        JsonObject file;
        file.set("formatVersion"sv, 1);
        file.set("items"sv, candidate);
        auto contents = file.serialized();
        if (!in_memory)
            TRY(writer(path(id), contents));
        entry->items = move(candidate);
        entry->bytes_in_use = bytes_in_use;
        return changes;
    }

    ByteString root;
    Writer writer;
    bool in_memory { false };
    bool is_sync { false };
    Core::EventLoop* loop { nullptr };
    Mutex mutex;
    ConditionVariable condition { mutex };
    Vector<Function<void()>> actions;
    bool stopping { false };
    HashMap<String, Entry> entries;
    NonnullRefPtr<Threading::Thread> worker;
};

ExtensionStorageStore::ExtensionStorageStore(ByteString root, Writer writer)
    : m_impl(make<Impl>(move(root), move(writer)))
{
}
ExtensionStorageStore::~ExtensionStorageStore() = default;

NonnullOwnPtr<ExtensionStorageStore> ExtensionStorageStore::create_session_store()
{
    auto store = make<ExtensionStorageStore>(ByteString {});
    store->m_impl->in_memory = true;
    return store;
}

NonnullOwnPtr<ExtensionStorageStore> ExtensionStorageStore::create_sync_store(ByteString profile_data, Writer writer)
{
    auto store = make<ExtensionStorageStore>(move(profile_data), move(writer));
    store->m_impl->is_sync = true;
    return store;
}

void ExtensionStorageStore::get(String const& id, Optional<Vector<String>> keys, Function<void(ErrorOr<JsonObject>)> callback)
{
    m_impl->enqueue<JsonObject>([impl = m_impl.ptr(), id, keys = move(keys)]() -> ErrorOr<JsonObject> { return Impl::select(*TRY(impl->load(id))->items, keys); }, move(callback));
}

void ExtensionStorageStore::set(String const& id, JsonObject items, bool unlimited, Function<void(ErrorOr<StorageChanges>)> callback)
{
    m_impl->enqueue<StorageChanges>([impl = m_impl.ptr(), id, items = move(items), unlimited]() -> ErrorOr<StorageChanges> {
        return impl->mutate(id, [&](JsonObject const& current) -> ErrorOr<StorageChanges> {
            StorageChanges changes;
            TRY(items.try_for_each_member([&](auto const& key, auto const& value) -> ErrorOr<void> {
                // Both the persistence file and the change event wrap each value in two objects.
                TRY(check_nesting_depth(value, JsonParser::max_nesting_depth - 2));
                auto previous = current.get(key);
                if (!previous.has_value() || previous->serialized() != value.serialized())
                    changes.set(key, ExtensionStorageChange { previous.has_value() ? Optional<JsonValue> { *previous } : OptionalNone {}, value });
                return {};
            }));
            return changes; }, unlimited);
    },
        move(callback));
}

void ExtensionStorageStore::remove(String const& id, Vector<String> keys, Function<void(ErrorOr<StorageChanges>)> callback)
{
    m_impl->enqueue<StorageChanges>([impl = m_impl.ptr(), id, keys = move(keys)]() -> ErrorOr<StorageChanges> {
        return impl->mutate(id, [&](JsonObject const& current) -> ErrorOr<StorageChanges> {
            StorageChanges changes;
            for (auto const& key : keys) {
                if (auto previous = current.get(key); previous.has_value())
                    changes.set(key, ExtensionStorageChange { *previous, {} });
            }
            return changes; }, true);
    },
        move(callback));
}

void ExtensionStorageStore::clear(String const& id, Function<void(ErrorOr<StorageChanges>)> callback)
{
    m_impl->enqueue<StorageChanges>([impl = m_impl.ptr(), id]() -> ErrorOr<StorageChanges> {
        return impl->mutate(id, [](JsonObject const& current) -> ErrorOr<StorageChanges> {
            StorageChanges changes;
            current.for_each_member([&](auto const& key, auto const& previous) { changes.set(key, ExtensionStorageChange { previous, {} }); });
            return changes; }, true);
    },
        move(callback));
}

void ExtensionStorageStore::bytes_in_use(String const& id, Optional<Vector<String>> keys, Function<void(ErrorOr<u64>)> callback)
{
    m_impl->enqueue<u64>([impl = m_impl.ptr(), id, keys = move(keys)]() -> ErrorOr<u64> {
        auto* entry = TRY(impl->load(id));
        return keys.has_value() ? Impl::size(Impl::select(*entry->items, keys)) : entry->bytes_in_use;
    },
        move(callback));
}

void ExtensionStorageStore::delete_storage(String const& id, Function<void(ErrorOr<void>)> callback)
{
    m_impl->enqueue<void>([impl = m_impl.ptr(), id]() -> ErrorOr<void> {
        if (!valid_id(id))
            return Error::from_string_literal("Invalid extension storage id");
        auto directory = impl->directory(id);
        if (!impl->in_memory && FileSystem::exists(directory))
            TRY(FileSystem::remove(directory, FileSystem::RecursionMode::Allowed));
        impl->entries.remove(id);
        return {};
    },
        move(callback));
}

}
