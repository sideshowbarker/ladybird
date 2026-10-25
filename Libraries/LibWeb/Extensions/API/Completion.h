/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Extensions {

WEB_API JS::Value settle_api_call(MessagingContext&, GC::Ptr<JS::FunctionObject>, GC::Ref<WebIDL::Promise>);

}
