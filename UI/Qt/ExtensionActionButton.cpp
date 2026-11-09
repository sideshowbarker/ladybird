/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ExtensionActionButton.h>
#include <UI/Qt/StringUtils.h>

#include <QFontMetrics>
#include <QPainter>
#include <QStyleOptionToolButton>
#include <QStylePainter>

namespace Ladybird {

static constexpr int badge_padding = 4;

ExtensionActionButton::ExtensionActionButton(QWidget* parent)
    : QToolButton(parent)
{
    setObjectName("ExtensionActionButton");
    setAutoRaise(true);
    setFixedSize(sizeHint());
    setFocusPolicy(Qt::StrongFocus);
}

void ExtensionActionButton::set_state(WebView::ExtensionActionState const& state, QIcon icon)
{
    m_state = state;
    m_icon = AK::move(icon);
    auto title = qstring_from_ak_string(state.title);
    setToolTip(title);
    setAccessibleName(title);
    setEnabled(state.enabled);
    update();
}

QFont ExtensionActionButton::badge_font() const
{
    auto badge_font = font();
    badge_font.setPointSizeF(7.5);
    badge_font.setBold(true);
    return badge_font;
}

QRect ExtensionActionButton::badge_rect() const
{
    if (m_state.badge_text.is_empty())
        return {};
    QFontMetrics metrics(badge_font());
    auto text_width = metrics.horizontalAdvance(qstring_from_ak_string(m_state.badge_text));
    auto badge_width = min(text_width + badge_padding, maximum_badge_width);
    auto badge_height = metrics.height() + 1;
    return { width() - badge_width - 1, height() - badge_height - 1, badge_width, badge_height };
}

void ExtensionActionButton::paintEvent(QPaintEvent*)
{
    QStyleOptionToolButton option;
    initStyleOption(&option);
    option.icon = {};

    QStylePainter painter(this);
    ChromeStyle::paint_control_frame(painter, *this);
    painter.drawComplexControl(QStyle::CC_ToolButton, option);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.save();
    if (!isEnabled())
        painter.setOpacity(0.45);
    auto icon_rect = QRect((width() - 20) / 2, (height() - 20) / 2, 20, 20);
    m_icon.paint(&painter, icon_rect, Qt::AlignCenter, underMouse() || isDown() ? QIcon::Active : QIcon::Normal);
    painter.restore();

    auto badge = badge_rect();
    if (badge.isEmpty())
        return;
    auto to_qcolor = [](Gfx::Color color) { return QColor(color.red(), color.green(), color.blue(), color.alpha()); };
    painter.setPen(Qt::NoPen);
    painter.setBrush(to_qcolor(m_state.badge_background_color));
    painter.drawRoundedRect(badge, 4, 4);
    painter.setFont(badge_font());
    painter.setPen(to_qcolor(m_state.badge_text_color));
    painter.setClipRect(badge.adjusted(1, 0, -1, 0));
    painter.drawText(badge, Qt::AlignCenter, qstring_from_ak_string(m_state.badge_text));
}

}
