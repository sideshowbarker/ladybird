/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Diagnostics.h>
#include <LibWeb/Extensions/ExtensionSet.h>

namespace Web::Extensions {

ExtensionSet& ExtensionSet::the()
{
    // One set per thread: a worker thread gets its own list over its own connection, and replaces it there, while the
    // main thread's readers iterate the main thread's. Nothing is shared, so nothing has to be locked, and a worker
    // thread's set goes with the thread.
    AK_IGNORE_DIAGNOSTIC("-Wexit-time-destructors", static thread_local ExtensionSet set);
    return set;
}

void ExtensionSet::set_extensions(Vector<ExtensionDescriptor> extensions)
{
    m_extensions = move(extensions);
    ++m_generation;
    if (on_changed)
        on_changed();
}

ExtensionDescriptor const* ExtensionSet::find(StringView id) const
{
    for (auto const& extension : m_extensions) {
        if (extension.id == id)
            return &extension;
    }
    return nullptr;
}

ExtensionDescriptor const* ExtensionSet::find_by_name(StringView name) const
{
    for (auto const& extension : m_extensions) {
        if (extension.name == name)
            return &extension;
    }
    return nullptr;
}

}
