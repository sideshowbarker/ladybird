/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/MemoryStream.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf8View.h>
#include <LibCompress/GenericZlib.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/ExtensionArchive.h>

namespace WebView {

// A zip entry's raw deflate data (RFC 1951), which has no zlib header or trailer.
class RawDeflateDecompressor final : public Compress::GenericZlibDecompressor {
public:
    static ErrorOr<NonnullOwnPtr<RawDeflateDecompressor>> create(MaybeOwned<Stream> stream)
    {
        auto buffer = TRY(FixedArray<u8>::create(16 * 1024));
        // A negative window size is how zlib is told the data is raw deflate, with the window that size's magnitude.
        auto zstream = TRY(new_z_stream(-15));
        return adopt_nonnull_own_or_enomem(new (nothrow) RawDeflateDecompressor(move(buffer), move(stream), zstream));
    }

private:
    RawDeflateDecompressor(FixedArray<u8> buffer, MaybeOwned<Stream> stream, z_stream* zstream)
        : GenericZlibDecompressor(move(buffer), move(stream), zstream)
    {
    }
};

static constexpr u32 local_header_signature = 0x04034b50;
static constexpr u32 central_header_signature = 0x02014b50;
static constexpr u32 end_of_directory_signature = 0x06054b50;
static constexpr size_t end_of_directory_size = 22;
static constexpr size_t central_header_size = 46;
static constexpr size_t local_header_size = 30;

static u16 read16(ReadonlyBytes bytes, size_t offset) { return bytes[offset] | (bytes[offset + 1] << 8); }
static u32 read32(ReadonlyBytes bytes, size_t offset) { return read16(bytes, offset) | (static_cast<u32>(read16(bytes, offset + 2)) << 16); }

static u32 crc32(ReadonlyBytes bytes)
{
    static auto const table = [] {
        Array<u32, 256> table {};
        for (u32 i = 0; i < 256; ++i) {
            u32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (0xedb88320 ^ (value >> 1)) : (value >> 1);
            table[i] = value;
        }
        return table;
    }();
    u32 crc = 0xffffffff;
    for (auto byte : bytes)
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static String refusal(StringView reason)
{
    return MUST(String::formatted("This isn't an extension archive Ladybird can install: {}", reason));
}

struct ArchiveEntry {
    ByteString path;
    bool is_directory { false };
    u16 method { 0 };
    u32 crc { 0 };
    u32 compressed_size { 0 };
    u32 size { 0 };
    u32 local_header_offset { 0 };
};

// A name is a relative path whose every part is a real name: not empty, not "." or "..", and with no backslash or
// drive letter that another system could read as a way out.
static bool is_safe_name(StringView name)
{
    if (name.is_empty() || name.starts_with('/') || name.contains('\\') || name.contains('\0'))
        return false;
    if (name.length() >= 2 && name[1] == ':')
        return false;
    auto trimmed = name.ends_with('/') ? name.substring_view(0, name.length() - 1) : name;
    for (auto part : trimmed.split_view('/', SplitBehavior::KeepEmpty)) {
        if (part.is_empty() || part == "."sv || part == ".."sv)
            return false;
    }
    return Utf8View { name }.validate();
}

static ErrorOr<Vector<ArchiveEntry>, String> read_directory(ReadonlyBytes archive)
{
    if (archive.size() < end_of_directory_size)
        return refusal("it's too short to be a zip file"sv);
    // The end of the central directory sits at the end, before a comment of up to 65,535 bytes.
    Optional<size_t> end;
    auto earliest = archive.size() > end_of_directory_size + 0xffff ? archive.size() - end_of_directory_size - 0xffff : 0;
    for (size_t offset = archive.size() - end_of_directory_size + 1; offset-- > earliest;) {
        if (read32(archive, offset) == end_of_directory_signature) {
            end = offset;
            break;
        }
    }
    if (!end.has_value())
        return refusal("it isn't a zip file"sv);
    auto count = read16(archive, *end + 10);
    auto directory_size = read32(archive, *end + 12);
    auto directory_offset = read32(archive, *end + 16);
    if (count == 0xffff || directory_offset == 0xffffffff)
        return refusal("it's a ZIP64 archive"sv);
    if (count > extension_archive_max_entries)
        return refusal("it has too many files"sv);
    if (static_cast<u64>(directory_offset) + directory_size > *end)
        return refusal("its directory is damaged"sv);

    Vector<ArchiveEntry> entries;
    HashTable<ByteString> names;
    u64 total = 0;
    size_t offset = directory_offset;
    for (size_t i = 0; i < count; ++i) {
        if (offset + central_header_size > *end || read32(archive, offset) != central_header_signature)
            return refusal("its directory is damaged"sv);
        auto made_by = read16(archive, offset + 4);
        auto flags = read16(archive, offset + 8);
        ArchiveEntry entry;
        entry.method = read16(archive, offset + 10);
        entry.crc = read32(archive, offset + 16);
        entry.compressed_size = read32(archive, offset + 20);
        entry.size = read32(archive, offset + 24);
        auto name_length = read16(archive, offset + 28);
        auto extra_length = read16(archive, offset + 30);
        auto comment_length = read16(archive, offset + 32);
        auto external_attributes = read32(archive, offset + 38);
        entry.local_header_offset = read32(archive, offset + 42);
        if (offset + central_header_size + name_length > *end)
            return refusal("its directory is damaged"sv);
        auto name = StringView { archive.slice(offset + central_header_size, name_length) };
        offset += central_header_size + name_length + extra_length + comment_length;

        if (flags & 1)
            return refusal("it's encrypted"sv);
        if (entry.method != 0 && entry.method != 8)
            return refusal("it uses a compression method other than deflate"sv);
        if (!is_safe_name(name))
            return refusal("a file name in it reaches outside the extension"sv);
        // A Unix archiver keeps the file mode in the high half; S_IFLNK is a symbolic link.
        if ((made_by >> 8) == 3 && ((external_attributes >> 16) & 0170000) == 0120000)
            return refusal("it holds a symbolic link"sv);
        entry.is_directory = name.ends_with('/');
        entry.path = name.ends_with('/') ? ByteString { name.substring_view(0, name.length() - 1) } : ByteString { name };
        // Two names that differ only in case would be one file on a case-insensitive file system.
        if (names.set(entry.path.to_lowercase()) != HashSetResult::InsertedNewEntry)
            return refusal("two files in it have the same name"sv);
        if (entry.size > extension_archive_max_entry_size)
            return refusal("a file in it is too large"sv);
        total += entry.size;
        if (total > extension_archive_max_total_size)
            return refusal("it's too large"sv);
        entries.append(move(entry));
    }
    return entries;
}

static ErrorOr<ByteBuffer, String> read_entry(ReadonlyBytes archive, ArchiveEntry const& entry)
{
    auto offset = static_cast<size_t>(entry.local_header_offset);
    if (offset + local_header_size > archive.size() || read32(archive, offset) != local_header_signature)
        return refusal("a file in it is damaged"sv);
    auto data_offset = offset + local_header_size + read16(archive, offset + 26) + read16(archive, offset + 28);
    if (static_cast<u64>(data_offset) + entry.compressed_size > archive.size())
        return refusal("a file in it is damaged"sv);
    auto compressed = archive.slice(data_offset, entry.compressed_size);
    ByteBuffer data;
    if (entry.method == 0) {
        // A stored entry is its own bytes, so a compressed size other than its size is a lie.
        if (entry.compressed_size != entry.size)
            return refusal("a file in it isn't the size it says"sv);
        auto copied = ByteBuffer::copy(compressed);
        if (copied.is_error())
            return refusal("a file in it is too large"sv);
        data = copied.release_value();
    } else {
        // Inflate no more than the size the entry declares, then see that nothing follows it.
        auto stream = make<FixedMemoryStream>(compressed);
        auto decompressor = RawDeflateDecompressor::create(MaybeOwned<Stream> { move(stream) });
        if (decompressor.is_error())
            return refusal("a file in it is damaged"sv);
        if (data.try_resize(entry.size).is_error())
            return refusal("a file in it is too large"sv);
        size_t filled = 0;
        while (filled < data.size()) {
            auto read = decompressor.value()->read_some(data.bytes().slice(filled));
            if (read.is_error())
                return refusal("a file in it is damaged"sv);
            if (read.value().is_empty())
                break;
            filled += read.value().size();
        }
        data.resize(filled);
        // A finished stream reports an error rather than more bytes when asked again, which is just what's wanted.
        u8 extra = 0;
        if (filled == entry.size && !decompressor.value()->is_eof()) {
            auto more = decompressor.value()->read_some({ &extra, 1 });
            if (!more.is_error() && !more.value().is_empty())
                return refusal("a file in it isn't the size it says"sv);
        }
    }
    if (data.size() != entry.size)
        return refusal("a file in it isn't the size it says"sv);
    if (crc32(data) != entry.crc)
        return refusal("a file in it is damaged"sv);
    return data;
}

ErrorOr<void> remove_extension_package(ByteString const& path)
{
    auto status = TRY(Core::System::lstat(path));
    if (!S_ISDIR(status.st_mode))
        return Core::System::unlink(path);
    Vector<ByteString> entries;
    TRY(Core::Directory::for_each_entry(path, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        entries.append(LexicalPath::join(path, entry.name).string());
        return IterationDecision::Continue;
    }));
    for (auto const& entry : entries)
        TRY(remove_extension_package(entry));
    return Core::System::rmdir(path);
}

ErrorOr<void, String> extract_extension_archive(ReadonlyBytes archive, ByteString const& destination)
{
    if (FileSystem::exists(destination))
        return "The extension's directory already exists"_string;
    if (archive.size() > extension_archive_max_file_size)
        return refusal("it's too large"sv);
    auto entries = TRY(read_directory(archive));

    // Everything goes into a directory beside the destination first, which becomes the destination only once every
    // file is in place.
    auto staging = ByteString::formatted("{}.{}", destination, AK::generate_random_uuid());
    auto io_error = [](Error const& error) { return MUST(String::formatted("Unable to extract the extension: {}", error)); };
    if (auto created = Core::Directory::create(staging, Core::Directory::CreateDirectories::Yes, 0700); created.is_error())
        return io_error(created.error());
    ArmedScopeGuard remove_staging = [&] { (void)remove_extension_package(staging); };
    for (auto const& entry : entries) {
        auto path = LexicalPath::join(staging, entry.path);
        if (entry.is_directory) {
            if (auto created = Core::Directory::create(path, Core::Directory::CreateDirectories::Yes, 0700); created.is_error())
                return io_error(created.error());
            continue;
        }
        auto data = TRY(read_entry(archive, entry));
        if (auto created = Core::Directory::create(path.parent(), Core::Directory::CreateDirectories::Yes, 0700); created.is_error())
            return io_error(created.error());
        auto file = Core::File::open(path.string(), Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew, 0600);
        if (file.is_error())
            return io_error(file.error());
        if (auto written = file.value()->write_until_depleted(data); written.is_error())
            return io_error(written.error());
    }
    if (auto renamed = FileSystem::rename_without_replacing(destination, staging); renamed.is_error())
        return io_error(renamed.error());
    remove_staging.disarm();
    return {};
}

}
