/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <QIcon>
#include <QLabel>
#include <QToolButton>
#include <QWidget>

namespace Ladybird {

class WebContentView;

// An extension's side panel, docked beside the page: a header with the extension's icon, the panel's title, and a
// close button, the panel's page below, and a handle on its left edge that resizes it.
class ExtensionSidePanel final : public QWidget {
    AK_ALLOC_WITH_KMALLOC;

public:
    static constexpr int default_width = 320;
    static constexpr int minimum_width = 200;

    // Shows the view the Hub made for the panel's page.
    ExtensionSidePanel(QWidget* parent, WebView::IsPrivate, int width, u64 hub_view_id);

    WebContentView& view() { return *m_view; }
    void set_header(QString const& title, QIcon const& icon);
    void set_panel_width(int);
    int panel_width() const { return m_width; }

    // The user pressed the close button.
    Function<void()> on_close_requested;
    // The user dragged the handle; the width is what the panel has then.
    Function<void(int)> on_width_changed;

private:
    virtual bool eventFilter(QObject*, QEvent*) override;
    void update_chrome_style();

    WebContentView* m_view { nullptr };
    QWidget* m_header { nullptr };
    QWidget* m_resize_handle { nullptr };
    QToolButton* m_icon { nullptr };
    QLabel* m_title { nullptr };
    QToolButton* m_close_button { nullptr };
    int m_width { default_width };
    Optional<int> m_drag_start_x;
    int m_drag_start_width { 0 };
};

}
