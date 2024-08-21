/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct ScrollFrame : public RefCounted<ScrollFrame> {
    i32 id { -1 };
    CSSPixelPoint own_offset;
    RefPtr<ScrollFrame const> parent;

    CSSPixelPoint cumulative_offset() const
    {
        if (parent)
            return parent->cumulative_offset() + own_offset;
        return own_offset;
    }
};

}