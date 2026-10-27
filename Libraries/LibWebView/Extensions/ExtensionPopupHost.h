/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGfx/Size.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct ExtensionPopupRequest {
    String extension_id;
    i64 tab_id { 0 };
    i64 window_id { 0 };
    URL::URL url;
    // Set by the host: which of the window's popups this is, so that a replaced one's late callbacks are ignored.
    u64 generation { 0 };
};

// A popup is sized to its page's content, within these bounds.
WEBVIEW_API Gfx::IntSize clamp_extension_popup_size(Gfx::IntSize);

// The popups of extensions' toolbar buttons: at most one per window, open until it loses focus, the user presses
// Escape, its page closes itself, the window's current tab changes, or its button is clicked again. The frontend only
// shows a popup's view; this decides when one opens and closes, and how big it is.
class WEBVIEW_API ExtensionPopupHost {
    AK_ALLOC_WITH_KMALLOC;

public:
    class Frontend {
    public:
        virtual ~Frontend() = default;
        // Shows a popup view for the request, loading its URL; false when the frontend can't.
        virtual bool show_extension_popup(ExtensionPopupRequest const&) = 0;
        virtual void resize_extension_popup(i64 window_id, Gfx::IntSize) = 0;
        virtual void close_extension_popup(i64 window_id) = 0;
        virtual ViewImplementation* extension_popup_view(i64 window_id) const = 0;
    };

    enum class CloseReason : u8 {
        FocusLost,
        Escape,
        WindowClose,
        ActiveTabChanged,
        Toggled,
        Replaced,
        ExtensionGone,
        TabClosed,
    };

    explicit ExtensionPopupHost(Frontend&);

    // Opens the popup, or closes it when this extension's popup is already open in the window. Another extension's
    // popup in the window closes first.
    void toggle(ExtensionPopupRequest);
    // Closes the window's popup, whichever it is.
    void close(i64 window_id, CloseReason);
    // A popup's own view reports its size or closes, naming its generation; a popup that was replaced is ignored.
    void close(i64 window_id, u64 generation, CloseReason);
    void content_size_changed(i64 window_id, u64 generation, Gfx::IntSize);
    void extension_gone(StringView extension_id);
    void tab_closed(i64 tab_id);

    bool is_open(i64 window_id) const { return m_popups.contains(window_id); }
    Optional<String> extension_in(i64 window_id) const;

    // The window whose button opened the popup a page is showing, and whether a page is an extension's popup.
    Optional<i64> window_for_page(WebContentPage const&) const;
    bool hosts_page(StringView extension_id, WebContentPage const&) const;

private:
    struct Popup {
        ExtensionPopupRequest request;
        Optional<Gfx::IntSize> size;
    };

    Optional<i64> window_showing(WebContentPage const&) const;

    Frontend& m_frontend;
    HashMap<i64, Popup> m_popups;
    u64 m_next_generation { 1 };
};

}
