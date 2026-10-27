/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebView/Extensions/ExtensionActionRegistry.h>

#include <QIcon>
#include <QToolButton>

namespace Ladybird {

// An extension's toolbar button: its icon, with its badge over the bottom-right corner, and its title as the tooltip.
class ExtensionActionButton final : public QToolButton {
    AK_ALLOC_WITH_KMALLOC;

public:
    // The badge's text is clipped to this width, rather than cut at a number of characters.
    static constexpr int maximum_badge_width = 27;

    explicit ExtensionActionButton(QWidget* parent);

    void set_state(WebView::ExtensionActionState const&, QIcon);
    QRect badge_rect() const;

private:
    virtual void paintEvent(QPaintEvent*) override;
    virtual QSize sizeHint() const override { return { 34, 34 }; }

    QFont badge_font() const;

    WebView::ExtensionActionState m_state;
    QIcon m_icon;
};

}
