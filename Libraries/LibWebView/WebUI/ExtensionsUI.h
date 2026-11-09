/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Random.h>
#include <LibWebView/Extensions/ExtensionArchive.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class ExtensionInstallRequests {
public:
    String begin(PendingExtensionInstall pending)
    {
        clear();
        m_token = generate_random_uuid();
        m_pending = move(pending);
        return m_token;
    }

    Optional<PendingExtensionInstall> take(StringView token)
    {
        if (!m_pending.has_value() || token != m_token)
            return {};
        m_token = {};
        return m_pending.release_value();
    }

    void cancel(StringView token)
    {
        if (token == m_token)
            clear();
    }

    // A request that's dropped takes its staged files with it.
    void clear()
    {
        m_token = {};
        if (m_pending.has_value() && m_pending->staged_package.has_value())
            (void)remove_extension_package(*m_pending->staged_package);
        m_pending.clear();
    }

private:
    String m_token;
    Optional<PendingExtensionInstall> m_pending;
};

class ExtensionsUI final
    : public WebUI
    , public ExtensionRegistryObserver {
public:
    static NonnullRefPtr<ExtensionsUI> create(WebContentClient& client, Web::PageId page_id, NonnullOwnPtr<IPC::Transport> transport, String host)
    {
        return adopt_ref(*new ExtensionsUI(client, page_id, move(transport), move(host)));
    }

private:
    ExtensionsUI(WebContentClient&, Web::PageId, NonnullOwnPtr<IPC::Transport>, String host);

    virtual void register_interfaces() override;
    virtual void extensions_changed() override;

    bool is_active() const;
    bool consume_user_activation();
    void send_snapshot();
    void send_result(StringView operation, String error = {});
    enum class Source {
        Directory,
        Archive,
    };
    // Asks the user for an unpacked extension's directory, or for an .xpi or .zip file, and installs what they chose.
    void load(Source);
    void install(JsonValue const&);

    ExtensionInstallRequests m_install_requests;
    u64 m_picker_generation { 0 };
};

}
