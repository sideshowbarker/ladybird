/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Extensions/ExtensionManifest.h>
#include <LibWebView/Forward.h>

namespace WebView {

// What a frontend shows for an extension's side panel in a window: the panel's page, and the header's title and icon.
struct ExtensionSidePanelRequest {
    String extension_id;
    i64 window_id { 0 };
    URL::URL url;
    String title;
    Vector<ExtensionManifest::IconPath> icon;
    // Set by the host: which of the window's panels this is, so that a replaced one's late callbacks are ignored.
    u64 generation { 0 };
};

// An extension's panel options, as the sidePanel API has them: the panel's page, as an absolute path within the
// extension, and whether the panel may open.
struct ExtensionSidePanelOptions {
    Optional<String> path;
    bool enabled { true };
};

// The side panels of extensions: one per window, docked beside the page, open until the user closes it, the extension
// closes it, its page closes itself, the window closes, or another extension's panel takes its place. A panel belongs
// to its window, not a tab: it stays across tab switches and navigations, showing the page the active tab resolves to.
// The frontend shows a panel's view; this decides when one opens, closes, and which page it shows.
class WEBVIEW_API ExtensionSidePanelHost {
    AK_ALLOC_WITH_KMALLOC;

public:
    class Frontend {
    public:
        virtual ~Frontend() = default;
        // Shows a panel view for the request, loading its URL; false when the frontend can't.
        virtual bool show_extension_side_panel(ExtensionSidePanelRequest const&) = 0;
        virtual void navigate_extension_side_panel(i64 window_id, URL::URL const&) = 0;
        virtual void close_extension_side_panel(i64 window_id) = 0;
        virtual ViewImplementation* extension_side_panel_view(i64 window_id) const = 0;
        // The windows and tabs the frontend has, and whether an extension may show in a window (a private one, e.g.).
        virtual Optional<i64> extension_window_of_tab(i64 tab_id) const = 0;
        virtual Optional<i64> extension_active_tab_of_window(i64 window_id) const = 0;
        virtual bool extension_may_show_in_window(StringView extension_id, i64 window_id) const = 0;
    };

    enum class CloseReason : u8 {
        UserClosed,
        Api,
        PageClosed,
        WindowClose,
        Replaced,
        ExtensionGone,
        Disabled,
    };

    // Which API set a per-tab override: a navigation clears sidebarAction's, and keeps sidePanel's.
    enum class OverrideSource : u8 {
        SidePanel,
        SidebarAction,
    };

    explicit ExtensionSidePanelHost(Frontend&);

    // The manifest's declaration becomes the extension's global options; set again (a reload, a re-enable), it replaces
    // whatever the extension set at runtime.
    void set_extension(String id, String name, ExtensionManifest::SidePanel);
    // Closes the extension's panels and forgets its options.
    void remove_extension(StringView id);
    void close_panels_of(StringView id, CloseReason);
    bool has_side_panel(StringView id) const;

    // Sets the global options (no tab id) or a tab's override; a field left out keeps its value. The options an
    // unknown extension resolves to are empty and disabled.
    void set_options(StringView id, Optional<i64> tab_id, Optional<String> path, Optional<bool> enabled, OverrideSource = OverrideSource::SidePanel);
    ExtensionSidePanelOptions options(StringView id, Optional<i64> tab_id) const;
    bool has_override(StringView id, i64 tab_id) const;
    void clear_override(StringView id, i64 tab_id);
    // The global options go back to the manifest's.
    void reset_options(StringView id);
    // The panel's page for a tab (or globally), as a URL within the extension; nothing without a page.
    Optional<String> panel_url(StringView id, Optional<i64> tab_id) const;
    void set_opens_on_action_click(StringView id, bool);
    bool opens_on_action_click(StringView id) const;
    // The header's title and icon: what the extension set for the tab, else globally, else the manifest's, else the
    // extension's name. A null value clears what was set at that level.
    void set_title(StringView id, Optional<i64> tab_id, Optional<String>);
    String title(StringView id, Optional<i64> tab_id) const;
    void set_icon(StringView id, Optional<i64> tab_id, Optional<Vector<ExtensionManifest::IconPath>>);
    Vector<ExtensionManifest::IconPath> icon(StringView id, Optional<i64> tab_id) const;

    // Opens the panel in the window, or in the tab's window showing what that tab resolves to; closes the extension's
    // own panel there. The errors are the ones extensions compare against, so their wording is fixed.
    ErrorOr<void, String> open(StringView id, Optional<i64> window_id, Optional<i64> tab_id);
    ErrorOr<void, String> close(StringView id, Optional<i64> window_id, Optional<i64> tab_id);
    // Opens the panel, or closes it when this extension's is the one open in the window.
    void toggle(StringView id, i64 window_id);

    // Closes the window's panel, whichever it is.
    void close(i64 window_id, CloseReason);
    // A panel's own view closes, naming its generation; a panel that was replaced is ignored.
    void close(i64 window_id, u64 generation, CloseReason);

    bool is_open(i64 window_id) const { return m_panels.contains(window_id); }
    bool is_open(StringView id, i64 window_id) const;
    Optional<String> extension_in(i64 window_id) const;

    // The window's active tab changed: the panel shows what the tab resolves to, keeping its document when that's the
    // same page, navigating when it's another, and closing where the panel is disabled.
    void active_tab_changed(i64 window_id, i64 tab_id);
    void tab_closed(i64 tab_id);
    void tab_navigated(i64 tab_id);

    // The panel open in a window, as test mode describes it.
    struct PanelState {
        String extension_id;
        URL::URL url;
        String title;
    };
    Optional<PanelState> state(i64 window_id) const;

    // The window whose panel a page is showing, and whether a page is an extension's panel.
    Optional<i64> window_for_page(WebContentPage const&) const;
    bool hosts_page(StringView extension_id, WebContentPage const&) const;

    // Fired with the extension, the window, the tab the panel was opened for (if any), and the panel page's path.
    Function<void(String const& extension_id, i64 window_id, Optional<i64> tab_id, String const& path)> on_opened;
    Function<void(String const& extension_id, i64 window_id, Optional<i64> tab_id, String const& path)> on_closed;

private:
    struct Override {
        Optional<String> path;
        Optional<bool> enabled;
        Optional<String> title;
        Optional<Vector<ExtensionManifest::IconPath>> icon;
        OverrideSource source { OverrideSource::SidePanel };
    };
    struct Entry {
        String id;
        String name;
        ExtensionManifest::SidePanel declaration;
        ExtensionSidePanelOptions global;
        HashMap<i64, Override> per_tab;
        bool opens_on_action_click { false };
        Optional<String> title;
        Optional<Vector<ExtensionManifest::IconPath>> icon;
    };
    struct Panel {
        String extension_id;
        URL::URL url;
        Optional<i64> tab_id;
        u64 generation { 0 };
    };

    Entry const* find(StringView id) const;
    Entry* find(StringView id);
    URL::URL url_for(StringView id, StringView path) const;
    Optional<URL::URL> resolved_url(Entry const&, i64 window_id) const;
    void show(Entry const&, i64 window_id, Optional<i64> tab_id, URL::URL);
    void resolve_open_panels_showing(StringView id);
    Optional<i64> window_showing(WebContentPage const&) const;

    Frontend& m_frontend;
    Vector<Entry> m_entries;
    HashMap<i64, Panel> m_panels;
    u64 m_next_generation { 1 };
};

}
