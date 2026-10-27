/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Extensions/API/Arguments.h>

namespace Web::Extensions {

bool is_api_integer(JS::Value value)
{
    return value.is_integral_number() && value.as_double() >= NumericLimits<i32>::min() && value.as_double() <= NumericLimits<i32>::max();
}

StringView api_type_name(JS::Value value)
{
    if (value.is_undefined())
        return "undefined"sv;
    if (value.is_null())
        return "null"sv;
    if (value.is_boolean())
        return "boolean"sv;
    if (value.is_number())
        return is_api_integer(value) ? "integer"sv : "number"sv;
    if (value.is_string())
        return "string"sv;
    if (value.is_function())
        return "function"sv;
    if (value.is_object() && is<JS::Array>(value.as_object()))
        return "array"sv;
    return "object"sv;
}

}
