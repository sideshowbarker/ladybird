/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Extensions/Namespace.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// The browser.runtime object: id, getURL(), getManifest(), lastError, onMessage, and sendMessage().
// https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/API/runtime
WEB_API GC::Ref<JS::Object> create_runtime(JS::Realm&, ExtensionDescriptor const&, NamespaceContext, EventListenerList& on_message_listeners);

}
