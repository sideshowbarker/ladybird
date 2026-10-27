/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Extensions {

// The webRequest type extensions see for a request ("main_frame", "script", e.g.), from what it fetches and who asked.
WEB_API StringView web_request_type_for(Fetch::Infrastructure::Request const&);

}
