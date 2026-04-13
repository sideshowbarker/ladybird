/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>
#include <UI/Accessibility/Linux/AtkBridge.h>
#include <UI/Accessibility/Linux/LadybirdAtkObject.h>
#include <UI/Accessibility/Linux/PrivateAccessibilityBus.h>

#include <atk/atk.h>
#include <cstring>
#include <gio/gio.h>

namespace {

StringView view(char const* s)
{
    return s ? StringView { s, strlen(s) } : StringView {};
}

WebView::AccessibilityNodeData make_node(i64 id, String role, String name = {})
{
    WebView::AccessibilityNodeData node;
    node.id = id;
    node.role = move(role);
    node.name = move(name);
    return node;
}

}

// One big test that owns the AtkBridge lifetime. Splitting into multiple cases would either require
// re-initializing the bridge per case (slow, and AtkUtil is globally registered once) or asserting
// against an AtkBridge::the() singleton assertion.
TEST_CASE(atk_bridge_initializes_and_exposes_tree_through_application_root)
{
    Ladybird::AtkBridge bridge;

    auto initialized = bridge.initialize();
    if (!initialized) {
        warnln("Skipping: AtkBridge failed to initialize (libatk-bridge-2.0 or at-spi2-registryd missing).");
        return;
    }

    EXPECT(bridge.is_initialized());
    EXPECT_NE(bridge.root_object(), nullptr);

    // Root is the application; at the point just after initialize() there is no document yet.
    EXPECT_EQ(atk_object_get_n_accessible_children(bridge.root_object()), 0);
    EXPECT_EQ(view(atk_object_get_name(bridge.root_object())), "Ladybird"sv);
    EXPECT_EQ(atk_object_get_role(bridge.root_object()), ATK_ROLE_APPLICATION);

    // Build a tiny tree: document → heading → text leaf.
    Ladybird::ladybird_atk_object_clear_cache();
    auto document = make_node(1, "document"_string, "Page Title"_string);
    document.child_ids = { 2 };
    document.language = "en"_string;

    auto heading = make_node(2, "heading"_string, "Hello"_string);
    heading.parent_id = 1;
    heading.child_ids = { 3 };
    heading.heading_level = 1;

    auto text = make_node(3, "text leaf"_string, "Hello"_string);
    text.parent_id = 2;

    WebView::AccessibilityTreeManager manager;
    manager.update_tree({ document, heading, text });

    bridge.set_active_tree(&manager, nullptr, nullptr);
    bridge.update_tree();

    // After update_tree, the application root reports exactly one child (the document root).
    EXPECT_EQ(atk_object_get_n_accessible_children(bridge.root_object()), 1);

    auto* document_root = bridge.document_root();
    EXPECT_NE(document_root, nullptr);
    EXPECT_EQ(view(atk_object_get_name(document_root)), "Page Title"sv);
    EXPECT_EQ(atk_object_get_role(document_root), ATK_ROLE_DOCUMENT_WEB);

    // Walk down: document's only accessible child is the heading, reachable via ref_accessible_child.
    auto* child = atk_object_ref_accessible_child(document_root, 0);
    EXPECT_NE(child, nullptr);
    EXPECT_EQ(view(atk_object_get_name(child)), "Hello"sv);
    EXPECT_EQ(atk_object_get_role(child), ATK_ROLE_HEADING);
    g_object_unref(child);

    // Document exposes its language via AtkDocument.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    EXPECT_EQ(view(atk_document_get_locale(ATK_DOCUMENT(document_root))), "en"sv);
#pragma GCC diagnostic pop

    // Focus change goes through both the state-change and focus-event signals. Attach watchers and verify.
    struct Recorder {
        bool state_change_fired { false };
        bool focus_event_fired { false };
    } recorder;

    auto state_handler = +[](AtkObject*, char const*, gboolean, gpointer user_data) {
        static_cast<Recorder*>(user_data)->state_change_fired = true;
    };
    auto focus_handler = +[](AtkObject*, gboolean, gpointer user_data) {
        static_cast<Recorder*>(user_data)->focus_event_fired = true;
    };

    auto* heading_obj = atk_object_ref_accessible_child(document_root, 0);
    g_signal_connect(heading_obj, "state-change", G_CALLBACK(state_handler), &recorder);
    g_signal_connect(heading_obj, "focus-event", G_CALLBACK(focus_handler), &recorder);

    bridge.notify_focus_changed(2);
    EXPECT(recorder.state_change_fired);
    EXPECT(recorder.focus_event_fired);

    g_object_unref(heading_obj);

    // The private bus is up and responding — connect a fresh GDBusConnection to it and verify a basic
    // call round-trips. We read the address from the well-known file because the bus is a private
    // member of AtkBridge.
    auto address_path = Ladybird::PrivateAccessibilityBus::address_file_path();
    FILE* f = fopen(address_path.characters(), "r");
    EXPECT_NE(f, nullptr);
    if (f) {
        char buffer[512] = {};
        auto bytes = fread(buffer, 1, sizeof(buffer) - 1, f);
        fclose(f);
        EXPECT(bytes > 0);

        GError* error = nullptr;
        auto* client = g_dbus_connection_new_for_address_sync(buffer,
            static_cast<GDBusConnectionFlags>(
                G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &error);
        EXPECT_EQ(error, nullptr);
        EXPECT_NE(client, nullptr);
        if (client)
            g_object_unref(client);
    }
}
