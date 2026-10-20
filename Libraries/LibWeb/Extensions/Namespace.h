/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// Which kind of context a namespace serves: a content script in a page's extension world, or one of the
// extension's own documents, such as its background page.
enum class NamespaceContext : u8 {
    ContentScript,
    ExtensionDocument,
};

// Defines "browser" and "chrome", one object, on the realm's global, and returns it.
WEB_API GC::Ref<JS::Object> install_namespace(JS::Realm&, ExtensionDescriptor const&, NamespaceContext, EventListenerList& on_message_listeners);

}
