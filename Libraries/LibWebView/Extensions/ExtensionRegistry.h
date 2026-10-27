/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ExtensionRegistry;

class WEBVIEW_API ExtensionRegistryObserver : public Weakable<ExtensionRegistryObserver> {
public:
    explicit ExtensionRegistryObserver(ExtensionRegistry&);
    virtual ~ExtensionRegistryObserver();
    virtual void extensions_changed() = 0;

private:
    WeakPtr<ExtensionRegistry> m_registry;
};

struct FailedExtension {
    String id;
    ByteString directory;
    String error;
};

struct PendingExtensionInstall {
    NonnullRefPtr<Extension> extension;
    bool is_reload;
    Vector<String> newly_declared_permissions;
};

// The extensions the user has loaded, in install order. Each one is persisted in Settings as its id, its
// directory, and whether it is enabled; the registry reloads them from disk at startup.
class WEBVIEW_API ExtensionRegistry : public Weakable<ExtensionRegistry> {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit ExtensionRegistry(Settings&);

    // Loads the extension in the directory, persisting it when it isn't installed yet. Loading an installed
    // directory again reloads it from disk and keeps its id.
    ErrorOr<NonnullRefPtr<Extension>, String> load_unpacked(ByteString directory);
    // Loads the extension in the directory without changing Settings or what's running, so its permissions can be
    // confirmed before install() persists them.
    ErrorOr<PendingExtensionInstall, String> prepare_unpacked(ByteString directory);
    // Adds the pending install's newly declared permissions to its saved grant, and replaces any loaded instance.
    NonnullRefPtr<Extension> install(PendingExtensionInstall);
    // Loads an installed extension from disk again, keeping its grant. On failure, it's unloaded and kept as failed.
    ErrorOr<NonnullRefPtr<Extension>, String> reload(StringView id);
    void remove(StringView id);
    void set_message_broker(ExtensionMessageBroker& broker) { m_message_broker = &broker; }
    // Each store whose items a removed extension loses: storage.local's and storage.sync's.
    void set_storage_store(ExtensionStorageStore& store) { m_storage_stores.append(&store); }
    void set_enabled(StringView id, bool enabled);
    void set_allowed_in_private(StringView id, bool);
    // What the extension has been granted, both API permissions and origin patterns; anything its manifest doesn't
    // declare counts for nothing.
    void set_granted(StringView id, Vector<String> api_permissions, Vector<String> origins);
    bool is_allowed_in_private(StringView id) const;

    // Loads every extension in Settings, keeping failures for display and retry.
    void restore_from_settings();

    RefPtr<Extension> find(StringView id) const;
    bool is_enabled(StringView id) const;
    bool may_access_resource(StringView id, StringView path, URL::URL const& document_url, IsPrivate = IsPrivate::No) const;
    bool may_create_context(StringView id, URL::URL const& document_url, bool is_top_level, bool inherited = false, IsPrivate = IsPrivate::No) const;
    Vector<NonnullRefPtr<Extension>> const& extensions() const { return m_extensions; }
    Vector<Web::Extensions::ExtensionDescriptor> enabled_descriptors(IsPrivate = IsPrivate::No) const;

    // The installed extensions whose directories didn't load, in install order.
    Vector<FailedExtension> const& failed_extensions() const { return m_failed_extensions; }

private:
    friend class ExtensionRegistryObserver;
    void notify_changed();
    void record_failure(String id, ByteString directory, String error);

    Settings& m_settings;
    ExtensionMessageBroker* m_message_broker { nullptr };
    Vector<ExtensionStorageStore*> m_storage_stores;
    Vector<NonnullRefPtr<Extension>> m_extensions;
    Vector<FailedExtension> m_failed_extensions;
    Vector<WeakPtr<ExtensionRegistryObserver>> m_observers;
};

}
