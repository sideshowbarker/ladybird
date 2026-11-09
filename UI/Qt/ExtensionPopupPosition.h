/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

namespace Ladybird {

// Where an extension's popup goes: below its button, left-aligned with it, but moved left or above it to stay on the
// screen.
QPoint extension_popup_position(QRect button_on_screen, QSize popup, QRect screen);

}
