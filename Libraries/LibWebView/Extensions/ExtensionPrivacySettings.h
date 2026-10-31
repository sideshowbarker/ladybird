/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// The privacy settings extensions may set, by their names under browser.privacy: network.networkPredictionEnabled,
// which turns DNS prefetching and preconnecting off and on for every page, and network.peerConnectionEnabled,
// network.webRTCIPHandlingPolicy, and websites.hyperlinkAuditingEnabled, which change nothing. Each extension's values
// are kept until it clears them or is disabled, reloaded, or removed; the most recently installed extension that set a
// value decides it. Nothing is kept across a restart.
class WEBVIEW_API ExtensionPrivacySettings {
    AK_ALLOC_WITH_KMALLOC;

public:
    // The installed extensions' ids, the earliest installed first.
    using InstallOrder = Function<Vector<String>()>;
    explicit ExtensionPrivacySettings(InstallOrder);

    static bool is_known(StringView name);
    static ReadonlySpan<StringView> names();

    // The setting as the extension sees it: its value, and the levelOfControl the extension has over it.
    JsonObject get(StringView extension_id, StringView name) const;
    // Whether the extension's value is then the one in effect.
    bool set(String const& extension_id, StringView name, JsonValue value);
    // Whether the extension had a value to clear.
    bool clear(StringView extension_id, StringView name);
    void remove_extension(StringView extension_id);
    // Drops the values of every extension that may not keep them.
    void retain_extensions(Function<bool(StringView extension_id)> const& may_keep);
    // The extensions with a value set, in no particular order.
    Vector<String> setters() const;

    bool network_prediction_enabled() const;

    // Called with a setting's name whenever its value in effect, or the extension that decides it, changes.
    Function<void(StringView name)> on_effective_value_change;

private:
    struct Effective {
        Optional<String> controller;
        JsonValue value;
        bool operator==(Effective const& other) const { return controller == other.controller && value.serialized() == other.value.serialized(); }
    };
    Effective effective(StringView name) const;
    Optional<size_t> install_index(StringView extension_id) const;
    // Announces the setting when its value in effect, or the extension deciding it, differs from what was last announced.
    void announce_if_changed(StringView name);

    InstallOrder m_install_order;
    // Each setting's values, by the extension that set them.
    HashMap<String, HashMap<String, JsonValue>> m_values;
    // What each setting was last announced as. The registry forgets a removed extension before this hears of it, so
    // that can't be recomputed from the install order then.
    HashMap<String, Effective> m_announced;
};

}
