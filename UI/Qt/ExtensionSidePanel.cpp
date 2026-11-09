/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ExtensionSidePanel.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Theme.h>
#include <UI/Qt/WebContentView.h>

namespace Ladybird {

static constexpr int HEADER_HEIGHT = 36;
static constexpr int HEADER_ICON_SIZE = 16;
static constexpr int RESIZE_HANDLE_WIDTH = 4;

ExtensionSidePanel::ExtensionSidePanel(QWidget* parent, WebView::IsPrivate is_private, int width, u64 hub_view_id)
    : QWidget(parent)
    , m_width(max(width, minimum_width))
{
    setAutoFillBackground(true);
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The handle lies along the panel's left edge, between the page and the panel.
    m_resize_handle = new QWidget(this);
    m_resize_handle->setFixedWidth(RESIZE_HANDLE_WIDTH);
    m_resize_handle->setCursor(Qt::SplitHCursor);
    m_resize_handle->installEventFilter(this);
    layout->addWidget(m_resize_handle);

    auto* column = new QWidget(this);
    auto* column_layout = new QVBoxLayout(column);
    column_layout->setContentsMargins(0, 0, 0, 0);
    column_layout->setSpacing(0);
    layout->addWidget(column, 1);

    m_header = new QWidget(column);
    m_header->setFixedHeight(HEADER_HEIGHT);
    auto* header_layout = new QHBoxLayout(m_header);
    header_layout->setContentsMargins(8, 0, 4, 0);
    header_layout->setSpacing(6);
    m_icon = new QToolButton(m_header);
    m_icon->setAutoRaise(true);
    m_icon->setEnabled(false);
    m_icon->setIconSize(QSize(HEADER_ICON_SIZE, HEADER_ICON_SIZE));
    m_icon->setFocusPolicy(Qt::NoFocus);
    header_layout->addWidget(m_icon, 0, Qt::AlignVCenter);
    m_title = new QLabel(m_header);
    m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header_layout->addWidget(m_title, 1, Qt::AlignVCenter);
    m_close_button = new QToolButton(m_header);
    m_close_button->setAutoRaise(true);
    m_close_button->setIconSize(QSize(HEADER_ICON_SIZE, HEADER_ICON_SIZE));
    m_close_button->setFocusPolicy(Qt::NoFocus);
    m_close_button->setToolTip("Close");
    connect(m_close_button, &QToolButton::clicked, this, [this] {
        if (on_close_requested)
            on_close_requested();
    });
    header_layout->addWidget(m_close_button, 0, Qt::AlignVCenter);
    column_layout->addWidget(m_header);

    m_view = new WebContentView(column, hub_view_id, WebContentViewInitialState { .is_private = is_private, .maximum_frames_per_second = 60.0, .display_id = {}, .owner_view_id = {}, .is_tab = false });
    column_layout->addWidget(m_view, 1);

    set_panel_width(m_width);
    update_chrome_style();
}

void ExtensionSidePanel::set_header(QString const& title, QIcon const& icon)
{
    m_title->setText(title);
    m_title->setToolTip(title);
    m_icon->setIcon(icon);
    m_icon->setVisible(!icon.isNull());
}

void ExtensionSidePanel::set_panel_width(int width)
{
    m_width = max(width, minimum_width);
    setFixedWidth(m_width);
}

// Dragging the handle moves the panel's left edge: the panel grows as the handle moves left.
bool ExtensionSidePanel::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_resize_handle)
        return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        if (mouse_event->button() != Qt::LeftButton)
            break;
        m_drag_start_x = mouse_event->globalPosition().toPoint().x();
        m_drag_start_width = m_width;
        return true;
    }
    case QEvent::MouseMove: {
        if (!m_drag_start_x.has_value())
            break;
        auto* mouse_event = static_cast<QMouseEvent*>(event);
        auto delta = *m_drag_start_x - mouse_event->globalPosition().toPoint().x();
        // The panel leaves at least as much of the window to the page as it takes itself.
        auto maximum_width = parentWidget() ? max(minimum_width, parentWidget()->width() / 2) : NumericLimits<int>::max();
        set_panel_width(clamp(m_drag_start_width + delta, minimum_width, maximum_width));
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (!m_drag_start_x.has_value())
            break;
        m_drag_start_x = {};
        if (on_width_changed)
            on_width_changed(m_width);
        return true;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void ExtensionSidePanel::update_chrome_style()
{
    auto palette = Theme::chrome_palette(*this);
    setPalette(palette);
    m_header->setAutoFillBackground(true);
    m_header->setPalette(palette);
    m_close_button->setIcon(create_chrome_icon(ChromeIcon::Close, palette, ChromeStyle::WindowActivity::Active, HEADER_ICON_SIZE));
}

}
