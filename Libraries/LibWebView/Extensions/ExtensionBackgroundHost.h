/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Timer.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/HeadlessWebView.h>

namespace WebView {

// Hosts the background page of every enabled extension that has one, each in a hidden headless view of its own,
// and so in a WebContent process of its own. The page is the one the extension's manifest names, or the generated
// page that loads its background scripts.
class WEBVIEW_API ExtensionBackgroundHost {
    AK_ALLOC_WITH_KMALLOC;

public:
    ExtensionBackgroundHost();
    ~ExtensionBackgroundHost();

    // Starts a background page for every enabled extension that has one and isn't hosted yet, and stops the pages
    // of extensions that are gone or disabled.
    void prepare(ExtensionRegistry const&);
    void sync(ExtensionRegistry const&);

    bool is_hosting(StringView extension_id) const { return m_views.contains(extension_id); }
    bool hosts_page(StringView extension_id, WebContentPage const&) const;

private:
    void start(Extension const&);
    void stop(StringView extension_id);

    ErrorOr<Core::AnonymousBuffer> theme();

    struct Background {
        NonnullOwnPtr<HeadlessWebView> view;
        u64 revision;
        NonnullRefPtr<Core::Timer> startup_timer;
    };
    HashMap<String, Background> m_views;
    HashMap<String, u64> m_instances;
    Optional<Core::AnonymousBuffer> m_theme;
};

}
