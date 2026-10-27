/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionLifecycle.h>

namespace WebView {

Optional<JsonObject> extension_installed_details(Optional<String> const& last_seen_version, String const& version)
{
    JsonObject details;
    if (!last_seen_version.has_value()) {
        details.set("reason"sv, "install"sv);
        return details;
    }
    if (*last_seen_version == version)
        return {};
    details.set("reason"sv, "update"sv);
    details.set("previousVersion"sv, *last_seen_version);
    return details;
}

bool extension_hears_browser_start(Optional<String> const& last_seen_version)
{
    return last_seen_version.has_value();
}

}
