/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionArchive.h>

#include <zlib.h>

namespace {

struct Fixture {
    Fixture()
        : root(LexicalPath::join(Core::StandardPaths::tempfile_directory(), AK::generate_random_uuid()).string())
        , destination(LexicalPath::join(root, "package"sv).string())
    {
        MUST(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes));
    }
    ~Fixture() { MUST(FileSystem::remove(root, FileSystem::RecursionMode::Allowed)); }
    String read(StringView path) const
    {
        auto file = MUST(Core::File::open(LexicalPath::join(destination, path).string(), Core::File::OpenMode::Read));
        return MUST(String::from_utf8(MUST(file->read_until_eof())));
    }
    // A refused archive leaves nothing behind: no destination, and nothing beside it.
    bool left_nothing() const
    {
        size_t entries = 0;
        MUST(Core::Directory::for_each_entry(root, Core::DirIterator::SkipParentAndBaseDir, [&](auto const&, auto const&) {
            ++entries;
            return IterationDecision::Continue;
        }));
        return entries == 0;
    }
    ByteString root;
    ByteString destination;
};

u32 crc32(ReadonlyBytes bytes)
{
    u32 crc = 0xffffffff;
    for (auto byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

// The data as raw deflate (RFC 1951), as a zip entry holds it.
ByteBuffer deflate_raw(ReadonlyBytes data)
{
    z_stream stream {};
    VERIFY(deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    auto out = MUST(ByteBuffer::create_uninitialized(deflateBound(&stream, data.size())));
    stream.next_in = const_cast<u8*>(data.data());
    stream.avail_in = data.size();
    stream.next_out = out.data();
    stream.avail_out = out.size();
    VERIFY(deflate(&stream, Z_FINISH) == Z_STREAM_END);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

struct Entry {
    ByteString name;
    ByteString data;
    bool deflate { false };
    Optional<u32> declared_size {};
    Optional<u32> crc {};
    bool symlink { false };
};

void put16(ByteBuffer& out, u16 value) { out.append(&value, 2); }
void put32(ByteBuffer& out, u32 value) { out.append(&value, 4); }

// A zip archive of the entries, each with the sizes and CRC it says it has unless the entry overrides them.
ByteBuffer make_archive(Vector<Entry> const& entries)
{
    ByteBuffer out;
    ByteBuffer directory;
    for (auto const& entry : entries) {
        auto data = entry.data.bytes();
        auto stored = entry.deflate ? deflate_raw(data) : MUST(ByteBuffer::copy(data));
        auto offset = static_cast<u32>(out.size());
        auto crc = entry.crc.value_or(crc32(data));
        auto size = entry.declared_size.value_or(data.size());
        u16 method = entry.deflate ? 8 : 0;
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, method);
        put32(out, 0);
        put32(out, crc);
        put32(out, stored.size());
        put32(out, size);
        put16(out, entry.name.length());
        put16(out, 0);
        out.append(entry.name.bytes());
        out.append(stored);

        put32(directory, 0x02014b50);
        put16(directory, (3 << 8) | 20);
        put16(directory, 20);
        put16(directory, 0);
        put16(directory, method);
        put32(directory, 0);
        put32(directory, crc);
        put32(directory, stored.size());
        put32(directory, size);
        put16(directory, entry.name.length());
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, (entry.symlink ? 0120777u : 0100644u) << 16);
        put32(directory, offset);
        directory.append(entry.name.bytes());
    }
    auto directory_offset = static_cast<u32>(out.size());
    out.append(directory);
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, entries.size());
    put16(out, entries.size());
    put32(out, directory.size());
    put32(out, directory_offset);
    put16(out, 0);
    return out;
}

// The archive lives until the call returns.
ErrorOr<void, String> extract(Fixture const& f, ByteBuffer archive)
{
    return WebView::extract_extension_archive(archive, f.destination);
}

}

TEST_CASE(stored_and_deflated_entries_extract)
{
    Fixture f;
    auto archive = make_archive({
        { "manifest.json", R"({"name":"x"})" },
        { "assets/", "" },
        { "assets/background.js", ByteString::repeated('a', 5000), true },
    });
    TRY_OR_FAIL(WebView::extract_extension_archive(archive, f.destination));
    EXPECT_EQ(f.read("manifest.json"sv), R"({"name":"x"})"sv);
    auto expected = ByteString::repeated('a', 5000);
    EXPECT_EQ(f.read("assets/background.js"sv), expected.view());
}

TEST_CASE(archive_refuses_traversal)
{
    for (auto name : { "../x"sv, "a/../../x"sv, "/x"sv, "C:/x"sv, "a\\x"sv, "./x"sv, "a//x"sv }) {
        Fixture f;
        auto result = extract(f, make_archive({ { "ok.js", "1" }, { name, "2" } }));
        EXPECT(result.is_error());
        EXPECT(f.left_nothing());
    }
}

TEST_CASE(archive_refuses_symlinks)
{
    Fixture f;
    Entry link { "link.js", "/etc/passwd" };
    link.symlink = true;
    EXPECT(extract(f, make_archive({ link })).is_error());
    EXPECT(f.left_nothing());
}

TEST_CASE(archive_refuses_duplicate_names)
{
    Fixture f;
    // Refused before anything is written, on a case-sensitive file system too.
    auto result = extract(f, make_archive({ { "A.js", "1" }, { "a.js", "2" } }));
    EXPECT(result.is_error() && result.error().contains("same name"sv));
    EXPECT(f.left_nothing());
}

TEST_CASE(archive_refuses_size_mismatch)
{
    // An entry that inflates past the size it declares is caught there, without inflating the rest.
    Fixture f;
    auto data = ByteString::repeated('b', 100000);
    Entry entry { "big.js", data, true };
    entry.declared_size = 10;
    // Its CRC is that of the bytes it claims, so only the size gives it away.
    entry.crc = crc32(data.bytes().slice(0, 10));
    auto result = extract(f, make_archive({ entry }));
    EXPECT(result.is_error() && result.error().contains("size"sv));
    EXPECT(f.left_nothing());
}

TEST_CASE(archive_refuses_bombs)
{
    {
        Fixture f;
        Entry entry { "huge.js", "x" };
        entry.declared_size = WebView::extension_archive_max_entry_size + 1;
        EXPECT(extract(f, make_archive({ entry })).is_error());
        EXPECT(f.left_nothing());
    }
    {
        Fixture f;
        Vector<Entry> entries;
        for (u32 i = 0; i < 5; ++i) {
            Entry entry { ByteString::formatted("{}.js", i), "x" };
            entry.declared_size = WebView::extension_archive_max_total_size / 4;
            entries.append(move(entry));
        }
        EXPECT(extract(f, make_archive(entries)).is_error());
        EXPECT(f.left_nothing());
    }
    {
        Fixture f;
        Vector<Entry> entries;
        for (size_t i = 0; i <= WebView::extension_archive_max_entries; ++i)
            entries.append({ ByteString::formatted("{}.js", i), "" });
        EXPECT(extract(f, make_archive(entries)).is_error());
        EXPECT(f.left_nothing());
    }
}

TEST_CASE(archive_refuses_bad_crc)
{
    Fixture f;
    Entry entry { "a.js", "hello" };
    entry.crc = 1234;
    EXPECT(extract(f, make_archive({ { "first.js", "1" }, entry })).is_error());
    EXPECT(f.left_nothing());
}

TEST_CASE(archive_refuses_what_isnt_one)
{
    Fixture f;
    EXPECT(WebView::extract_extension_archive("not a zip file"sv.bytes(), f.destination).is_error());
    auto archive = make_archive({ { "a.js", "hello" } });
    EXPECT(WebView::extract_extension_archive(archive.bytes().slice(0, archive.size() - 30), f.destination).is_error());
    EXPECT(f.left_nothing());
}

TEST_CASE(archive_refuses_a_stored_entry_whose_sizes_differ)
{
    // A stored entry is its own bytes, so a compressed size other than its size is a lie, refused before any copy.
    Fixture f;
    Entry entry { "stored.js", ByteString::repeated('s', 100000) };
    entry.declared_size = 10;
    entry.crc = crc32(entry.data.bytes().slice(0, 10));
    auto result = extract(f, make_archive({ entry }));
    EXPECT(result.is_error() && result.error().contains("size"sv));
    EXPECT(f.left_nothing());
}
