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

class MessagingContext;

// The sidePanel namespace: an extension's side panel options, global and per tab, and opening and closing the panel.
GC::Ref<JS::Object> create_side_panel(JS::Realm&, MessagingContext&);

// Whether the calling document has the user activation the panel-opening calls need.
bool has_user_activation_for_side_panel(MessagingContext&);

}
