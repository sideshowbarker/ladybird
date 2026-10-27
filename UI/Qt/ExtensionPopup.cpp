/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ExtensionPopup.h>
#include <UI/Qt/WebContentView.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QVBoxLayout>

namespace Ladybird {

ExtensionPopup::ExtensionPopup(QWidget& anchor, WebView::IsPrivate is_private)
    : QWidget(anchor.window(), Qt::Popup)
    , m_anchor(&anchor)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_view = new WebContentView(this, {}, WebContentViewInitialState { .is_private = is_private, .maximum_frames_per_second = 60.0, .display_id = {}, .owner_view = nullptr });
    m_view->installEventFilter(this);
    layout->addWidget(m_view);
    // The popup starts at the smallest size, and grows to its content as the page reports it.
    resize_to({ 25, 25 });
}

void ExtensionPopup::resize_to(Gfx::IntSize size)
{
    resize(size.width(), size.height());
    place();
}

void ExtensionPopup::place()
{
    if (!m_anchor)
        return;
    auto button = QRect(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
    auto* screen = m_anchor->screen();
    auto available = screen ? screen->availableGeometry() : QRect(button.topLeft(), QSize(width(), height()));
    move(extension_popup_position(button, size(), available));
}

void ExtensionPopup::dismiss()
{
    m_dismissed = true;
    hide();
}

void ExtensionPopup::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (!m_dismissed && on_close)
        on_close(WebView::ExtensionPopupHost::CloseReason::FocusLost);
}

void ExtensionPopup::mousePressEvent(QMouseEvent* event)
{
    // Qt closes a popup on a press outside it, then replays the press to the widget underneath. A press on the popup's
    // own button only closes it, so that the button's click doesn't open it again.
    auto on_anchor = m_anchor && QRect(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size()).contains(event->globalPosition().toPoint());
    setAttribute(Qt::WA_NoMouseReplay, on_anchor);
    QWidget::mousePressEvent(event);
}

bool ExtensionPopup::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_view && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (on_close)
            on_close(WebView::ExtensionPopupHost::CloseReason::Escape);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

}
