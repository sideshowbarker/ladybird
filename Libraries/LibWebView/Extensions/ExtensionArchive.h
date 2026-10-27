/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <LibWebView/Export.h>

namespace WebView {

// The most an extension archive may hold. The largest extension anyone is likely to install (uBlock Origin) unpacks to
// about 15 MB in some 2,000 files.
static constexpr size_t extension_archive_max_entries = 10'000;
static constexpr u64 extension_archive_max_entry_size = 64 * MiB;
static constexpr u64 extension_archive_max_total_size = 256 * MiB;
// An archive file larger than this can't hold an extension within the limits above, and isn't read at all.
static constexpr u64 extension_archive_max_file_size = 320 * MiB;

// Extracts a zip archive (an .xpi, or a .zip of an unpacked extension) into the destination directory, which must not
// exist yet. Only stored and deflated entries are read, and each one's CRC is checked. Nothing is written outside the
// destination, and an archive that's refused leaves nothing behind: one with an entry whose name climbs out of the
// destination or is absolute, a symbolic link, two entries whose names differ only in case, an entry larger than it
// says, or more entries or bytes than the limits above.
WEBVIEW_API ErrorOr<void, String> extract_extension_archive(ReadonlyBytes, ByteString const& destination);

// Deletes an extracted package and everything in it, removing a symbolic link inside it rather than anything it points
// to.
WEBVIEW_API ErrorOr<void> remove_extension_package(ByteString const& path);

}
