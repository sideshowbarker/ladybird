/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The ExtensionInfo management.getSelf() gives a running extension, with the fields extensions read: installType is
// "normal" for one installed from an archive, and "development" for one loaded from a directory.
WEBVIEW_API JsonObject extension_info_for(Extension const&, bool installed_from_package);

}
