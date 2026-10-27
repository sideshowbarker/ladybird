/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibJS/Runtime/Value.h>

namespace Web::Extensions {

// An API integer is a signed 32-bit one, and any other number is just a number.
bool is_api_integer(JS::Value);

// The name an extension API error gives a value's type: "integer", "string", "array", and so on.
StringView api_type_name(JS::Value);

}
