/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>

#include <zlib.h>

// Builds zip archives in memory for the extension archive and install tests.
namespace {

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

}
