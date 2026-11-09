/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>
#include <LibGfx/Size.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Extensions/ExtensionPopupHost.h>
#include <UI/Qt/ExtensionPopupPosition.h>

#include <QPointer>
#include <QWidget>

namespace Ladybird {

class WebContentView;

// The surface an extension's popup shows in: a Qt popup below its button, holding the popup page's view. Qt closes a
// popup when the user clicks outside it or another window takes focus; Escape closes it too.
class ExtensionPopup final : public QWidget {
    AK_ALLOC_WITH_KMALLOC;

public:
    // Shows the view the Hub made for the popup's page, below its button.
    ExtensionPopup(QWidget& anchor, WebView::IsPrivate, u64 hub_view_id, i64 window_id);

    WebContentView& view() { return *m_view; }
    i64 window_id() const { return m_window_id; }

    void resize_to(Gfx::IntSize);
    // Closes the popup without telling on_close, for a close the host asked for itself.
    void dismiss();

    Function<void(WebView::ExtensionPopupHost::CloseReason)> on_close;

private:
    virtual void hideEvent(QHideEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

    void place();

    QPointer<QWidget> m_anchor;
    WebContentView* m_view { nullptr };
    i64 m_window_id { 0 };
    bool m_dismissed { false };
};

}
