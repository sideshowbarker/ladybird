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

GC::Ref<JS::Object> create_i18n(JS::Realm&, MessagingContext&);

}
