/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ExtensionPopupPosition.h>

namespace Ladybird {

QPoint extension_popup_position(QRect button_on_screen, QSize popup, QRect screen)
{
    auto x = button_on_screen.left();
    if (x + popup.width() > screen.right() + 1)
        x = screen.right() + 1 - popup.width();
    if (x < screen.left())
        x = screen.left();

    auto y = button_on_screen.bottom() + 1;
    if (y + popup.height() > screen.bottom() + 1 && button_on_screen.top() - popup.height() >= screen.top())
        y = button_on_screen.top() - popup.height();
    return { x, y };
}

}
