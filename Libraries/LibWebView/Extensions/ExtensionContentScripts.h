/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Export.h>

namespace WebView {

// The content scripts extensions have registered with contentScripts.register(). Each belongs to the extension context
// that registered it, named by the page process hosting it (the owner) and its id there, and goes when that context
// does. None is kept across a restart.
class WEBVIEW_API ExtensionContentScriptRegistrations {
    AK_ALLOC_WITH_KMALLOC;

public:
    // Adds the script, and returns the handle its RegisteredContentScript unregisters it by.
    u64 add(String const& extension_id, void const* owner, u64 context_id, Web::Extensions::ContentScriptDescriptor);
    // Whether the extension had a registration with the handle, which is then gone.
    bool remove(StringView extension_id, u64 handle);
    // Drops what the context registered, and returns the extensions whose scripts that changed.
    Vector<String> context_destroyed(void const* owner, u64 context_id);
    // Drops what every context of the owner registered, and returns the extensions whose scripts that changed.
    Vector<String> owner_destroyed(void const* owner);
    void remove_extension(StringView extension_id);
    // The extension's registered scripts, in the order they were registered, each with the id "contentScripts:<handle>".
    Vector<Web::Extensions::ContentScriptDescriptor> scripts(StringView extension_id) const;

private:
    struct Registration {
        String extension_id;
        void const* owner { nullptr };
        u64 context_id { 0 };
        u64 handle { 0 };
        Web::Extensions::ContentScriptDescriptor script;
    };
    Vector<String> remove_matching(Function<bool(Registration const&)> const&);

    Vector<Registration> m_registrations;
    u64 m_next_handle { 1 };
};

}
