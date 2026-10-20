/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// The enabled extensions this process knows about, as the UI process last sent them.
class WEB_API ExtensionSet {
public:
    static ExtensionSet& the();

    void set_extensions(Vector<ExtensionDescriptor>);
    Function<void()> on_changed;

    ExtensionDescriptor const* find(StringView id) const;
    ExtensionDescriptor const* find_by_name(StringView name) const;
    ReadonlySpan<ExtensionDescriptor> extensions() const { return m_extensions; }

    // Bumps every time the set is replaced, so a holder of per-extension state can tell it is stale.
    u64 generation() const { return m_generation; }

private:
    Vector<ExtensionDescriptor> m_extensions;
    u64 m_generation { 0 };
};

}
