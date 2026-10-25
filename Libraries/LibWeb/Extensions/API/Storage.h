/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

GC::Ref<JS::Object> create_storage(JS::Realm&, MessagingContext&);
JS::Value storage_value_from_json(JS::Realm&, JsonValue const&);

}
