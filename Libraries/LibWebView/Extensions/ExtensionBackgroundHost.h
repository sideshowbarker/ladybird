/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Timer.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/ExtensionBackgroundLifetime.h>
#include <LibWebView/Forward.h>
#include <LibWebView/HeadlessWebView.h>

namespace WebView {

// Hosts the background page of every enabled extension that has one, each in a hidden headless view of its own,
// and so in a WebContent process of its own. The page is the one the extension's manifest names, or the generated
// page that loads its background scripts. A page that isn't persistent unloads when it has nothing to do, and starts
// again when something wakes it.
class WEBVIEW_API ExtensionBackgroundHost {
    AK_ALLOC_WITH_KMALLOC;

public:
    ExtensionBackgroundHost();
    ~ExtensionBackgroundHost();

    // Starts the background page of every enabled extension that has one and isn't known yet, so it can register its
    // listeners, and stops the pages of extensions that are gone, disabled, or reloaded. prepare() only does the
    // bookkeeping, so that messages wait for the pages; sync() also makes their views.
    void prepare(ExtensionRegistry const&);
    void sync(ExtensionRegistry const&);

    // Starts a stopped page, or keeps a suspending one; returns whether the page is starting now.
    bool wake(StringView extension_id);
    void activity(StringView extension_id, ExtensionBackgroundLifetime::FromPage);
    void suspend_acknowledged(StringView extension_id, u64 context_id);
    // For tests: how long one extension's page may sit idle.
    void set_idle_timeout(StringView extension_id, AK::Duration);

    bool is_hosting(StringView extension_id) const;
    bool hosts_page(StringView extension_id, WebContentPage const&) const;

private:
    struct Background {
        AK_ALLOC_WITH_KMALLOC;

        String id;
        u64 revision { 0 };
        URL::URL url;
        OwnPtr<ExtensionBackgroundLifetime> lifetime;
        OwnPtr<HeadlessWebView> view;
        RefPtr<Core::Timer> startup_timer;
        RefPtr<Core::Timer> idle_timer;
        RefPtr<Core::Timer> suspend_timer;
        // Counts the views made for the page, so that a task bound to one view never acts on its replacement.
        u64 view_generation { 0 };
        // The page's contexts that haven't yet acknowledged that their runtime.onSuspend listeners have run.
        HashTable<u64> pending_suspend_acknowledgments;
    };

    NonnullOwnPtr<Background> create_background(Extension const&);
    void start_view(Background&);
    void destroy_view(Background&);
    void forget(String const& extension_id);

    ErrorOr<Core::AnonymousBuffer> theme();

    HashMap<String, NonnullOwnPtr<Background>> m_backgrounds;
    bool m_views_may_start { false };
    Optional<Core::AnonymousBuffer> m_theme;
};

}
