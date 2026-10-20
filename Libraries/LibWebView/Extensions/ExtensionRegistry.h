/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The extensions the user has loaded, in install order. Each one is persisted in Settings as its id, its
// directory, and whether it is enabled; the registry reloads them from disk at startup.
class WEBVIEW_API ExtensionRegistry {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit ExtensionRegistry(Settings&);

    // Loads the extension in the directory, persisting it when it isn't installed yet. Loading an installed
    // directory again reloads it from disk and keeps its id.
    ErrorOr<NonnullRefPtr<Extension>, String> load_unpacked(ByteString directory);
    void remove(StringView id);
    void set_enabled(StringView id, bool enabled);

    // Loads every extension in Settings, dropping the ones whose directory no longer loads.
    void restore_from_settings();

    RefPtr<Extension> find(StringView id) const;
    bool is_enabled(StringView id) const;
    Vector<NonnullRefPtr<Extension>> const& extensions() const { return m_extensions; }
    Vector<Web::Extensions::ExtensionDescriptor> enabled_descriptors() const;

    // Called after every change to the set of loaded or enabled extensions.
    Function<void()> on_changed;

private:
    void notify_changed();

    Settings& m_settings;
    Vector<NonnullRefPtr<Extension>> m_extensions;
};

}
