/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/AccessibilityTreeManager.h>
#include <UI/Accessibility/Linux/GObjectPtr.h>
#include <UI/Accessibility/Linux/LadybirdAtkObject.h>
#include <UI/Accessibility/Linux/PrivateAccessibilityBus.h>

#include <atk/atk.h>
#include <glib.h>
#include <pthread.h>
#include <sys/types.h>

namespace Ladybird {

// Manages a private ATK-to-AT-SPI2 accessibility bridge on a separate D-Bus bus. Provides full AT-SPI2 interface
// support (Collection, Hypertext, Document, Cache) independently of any UI toolkit. The toolkit layer is responsible
// for pumping the GLib default context (via pump_default_context) and scheduling deferred cache activation (via
// activate_cache) using its own event loop.
class AtkBridge {
public:
    AtkBridge();
    ~AtkBridge();

    AtkBridge(AtkBridge const&) = delete;
    AtkBridge& operator=(AtkBridge const&) = delete;

    // Initialize the bridge: spawn private bus, register AtkUtil, load libatk-bridge-2.0, call
    // atk_bridge_adaptor_init(). Must be called after the toolkit’s own AT-SPI2 bridge has connected to the normal
    // bus, so that the AT_SPI_BUS_ADDRESS override only affects our bridge.
    bool initialize();

    // Set the accessibility tree manager and action callbacks for the currently active view.
    void set_active_tree(WebView::AccessibilityTreeManager const* manager, AccessibilityActionCallback* action_callback,
        AccessibilityTextActionCallback* text_action_callback = nullptr);

    // Update the ATK tree from the shared AccessibilityTreeManager. Called when the tree changes.
    void update_tree();

    // Notify of a focus change within the tree.
    void notify_focus_changed(i64 node_id);

    // Pump the GLib default main context to process pending cache idle callbacks. The toolkit layer should call this
    // periodically (e.g. every 50ms) from its own event loop.
    void pump_default_context();

    // Activate the bridge’s SpiCache. Must be called after registryd has had time to start (~200ms after
    // initialize()). The toolkit layer should schedule this as a deferred callback.
    void activate_cache();

    AtkObject* root_object() const { return m_root_object.get(); }
    AtkObject* document_root() const { return m_document_root; }
    bool is_initialized() const { return m_initialized; }

    static AtkBridge& the();

private:
    PrivateAccessibilityBus m_private_bus;
    void* m_atk_bridge_lib { nullptr };
    // Freshly g_object_new'd in initialize(); owned here so it is unref'd when AtkBridge is destroyed. The
    // GObjectPtr destructor runs after ~AtkBridge() finishes, by which time atk_bridge_adaptor_cleanup has
    // already released the bridge library's own references to this root.
    GObjectPtr<AtkObject> m_root_object;
    // Non-owning observer pointer into the per-node AtkObject cache in LadybirdAtkObject.cpp. That cache
    // holds the reference; it is cleared (and its entries unref'd) in ladybird_atk_object_clear_cache().
    AtkObject* m_document_root { nullptr };
    WebView::AccessibilityTreeManager const* m_manager { nullptr };
    AccessibilityActionCallback* m_action_callback { nullptr };
    AccessibilityTextActionCallback* m_text_action_callback { nullptr };
    pid_t m_registryd_pid { -1 };
    GMainLoop* m_bridge_main_loop { nullptr };
    pthread_t m_bridge_thread {};
    bool m_bridge_thread_running { false };
    bool m_initialized { false };
};

}
