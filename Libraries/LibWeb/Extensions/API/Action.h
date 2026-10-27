/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// browser.action in manifest version 3, or browser.browserAction in version 2: the extension's toolbar button.
GC::Ref<JS::Object> create_action(JS::Realm&, MessagingContext&);

}
