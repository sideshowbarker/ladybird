/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionContentScripts.h>

namespace WebView {

u64 ExtensionContentScriptRegistrations::add(String const& extension_id, void const* owner, u64 context_id, Web::Extensions::ContentScriptDescriptor script)
{
    auto handle = m_next_handle++;
    script.id = MUST(String::formatted("contentScripts:{}", handle));
    m_registrations.append({ extension_id, owner, context_id, handle, move(script) });
    return handle;
}

bool ExtensionContentScriptRegistrations::remove(StringView extension_id, u64 handle)
{
    return !remove_matching([&](auto const& registration) { return registration.extension_id == extension_id && registration.handle == handle; }).is_empty();
}

Vector<String> ExtensionContentScriptRegistrations::context_destroyed(void const* owner, u64 context_id)
{
    return remove_matching([&](auto const& registration) { return registration.owner == owner && registration.context_id == context_id; });
}

Vector<String> ExtensionContentScriptRegistrations::owner_destroyed(void const* owner)
{
    return remove_matching([&](auto const& registration) { return registration.owner == owner; });
}

void ExtensionContentScriptRegistrations::remove_extension(StringView extension_id)
{
    remove_matching([&](auto const& registration) { return registration.extension_id == extension_id; });
}

Vector<Web::Extensions::ContentScriptDescriptor> ExtensionContentScriptRegistrations::scripts(StringView extension_id) const
{
    Vector<Web::Extensions::ContentScriptDescriptor> scripts;
    for (auto const& registration : m_registrations) {
        if (registration.extension_id == extension_id)
            scripts.append(registration.script);
    }
    return scripts;
}

Vector<String> ExtensionContentScriptRegistrations::remove_matching(Function<bool(Registration const&)> const& predicate)
{
    Vector<String> changed;
    m_registrations.remove_all_matching([&](auto const& registration) {
        if (!predicate(registration))
            return false;
        if (!changed.contains_slow(registration.extension_id))
            changed.append(registration.extension_id);
        return true;
    });
    return changed;
}

}
