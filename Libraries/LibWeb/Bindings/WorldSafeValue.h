/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Bindings {

WEB_API JS::Value world_safe_value(JS::Realm&, JS::Value);
WEB_API JS::Value cached_world_safe_value(JS::Realm&, JS::Value);
WEB_API Variant<GC::Ref<DOM::Event>, Empty> world_safe_event(JS::Realm&, Variant<GC::Ref<DOM::Event>, Empty>);

}
