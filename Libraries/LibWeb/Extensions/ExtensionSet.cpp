/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Extensions/ExtensionSet.h>

namespace Web::Extensions {

ExtensionSet& ExtensionSet::the()
{
    static ExtensionSet* set = new ExtensionSet;
    return *set;
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
