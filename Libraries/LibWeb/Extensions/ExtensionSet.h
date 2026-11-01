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
#include <AK/kmalloc.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// The enabled extensions this thread knows about, as Hub last sent them over the connection the thread handles: a
// page's for the main thread, a worker's for the worker's thread.
class WEB_API ExtensionSet {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ExtensionSet& the();

    void set_extensions(Vector<ExtensionDescriptor>);
    // Replaces the content scripts an extension in the set registered at runtime, which change without the rest of it.
    void set_registered_content_scripts(StringView id, Vector<ContentScriptDescriptor>);
    Function<void()> on_changed;

    ExtensionDescriptor const* find(StringView id) const;
    ExtensionDescriptor const* find_by_name(StringView name) const;
    ReadonlySpan<ExtensionDescriptor> extensions() const { return m_extensions; }

    // Bumps every time the set or an extension in it changes, so a holder of per-extension state can tell it is stale.
    u64 generation() const { return m_generation; }

private:
    Vector<ExtensionDescriptor> m_extensions;
    u64 m_generation { 0 };
};

}
