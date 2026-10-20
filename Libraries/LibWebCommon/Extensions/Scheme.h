/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Web::Extensions {

// The URL scheme of an extension's own pages and resources: ladybird-extension://<id>/<path>.
constexpr StringView scheme = "ladybird-extension"sv;

// The generated page that hosts an extension's background scripts.
constexpr StringView generated_background_page_path = "/_generated_background_page.html"sv;

}
