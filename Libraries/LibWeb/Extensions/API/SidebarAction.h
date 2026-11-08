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

// The sidebarAction namespace: the panel the manifest's sidebar_action declares, its title and icon, and opening,
// closing, and toggling it in the current window.
GC::Ref<JS::Object> create_sidebar_action(JS::Realm&, MessagingContext&);

}
