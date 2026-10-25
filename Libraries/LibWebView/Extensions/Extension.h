/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibURL/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/ExtensionManifest.h>

namespace WebView {

// An unpacked extension loaded from a directory: its manifest, its directory, and the descriptor that WebContent
// processes get (with every content script's source already read).
class WEBVIEW_API Extension : public RefCounted<Extension> {
public:
    // Returns a user-facing error when the manifest is invalid or a file it names can't be read.
    static ErrorOr<NonnullRefPtr<Extension>, String> load_unpacked(ByteString directory, String id, Optional<Vector<String>> granted_permissions = {});

    void renew_instance();

    String const& id() const { return m_id; }
    ByteString const& directory() const { return m_directory; }
    ExtensionManifest const& manifest() const { return m_manifest; }
    Web::Extensions::ExtensionDescriptor const& descriptor() const { return m_descriptor; }

    // Whether a page at the initiator URL may load the resource at the URL path (which begins with "/").
    bool is_web_accessible(StringView url_path, URL::URL const& initiator) const;

    // Lexically resolve a URL path. Use open_resource() to enforce filesystem containment when reading a file.
    ErrorOr<ByteString> resolve_resource_path(StringView url_path) const;

    // Open a regular file and verify its location before returning any of its bytes.
    ErrorOr<NonnullOwnPtr<Core::File>> open_resource(StringView url_path) const;

private:
    Extension(ByteString directory, Core::Directory, String id, ExtensionManifest, Web::Extensions::ExtensionDescriptor);

    ByteString m_directory;
    Core::Directory m_resource_directory;
    String m_id;
    ExtensionManifest m_manifest;
    Web::Extensions::ExtensionDescriptor m_descriptor;
};

}
