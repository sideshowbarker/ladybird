/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// A permissions.Permissions object: API permission names and origin patterns.
struct ExtensionPermissionSet {
    Vector<String> permissions;
    Vector<String> origins;

    bool is_empty() const { return permissions.is_empty() && origins.is_empty(); }
    JsonObject to_json() const;
    static ExtensionPermissionSet from_json(JsonObject const&);
};

// What permissions.getAll() reports: the granted API permissions, and the origins the extension may reach, which are
// its host permissions, its granted optional ones, and its content scripts' match patterns.
WEBVIEW_API ExtensionPermissionSet extension_active_permissions(Extension const&);
WEBVIEW_API bool extension_has_permissions(Extension const&, ExtensionPermissionSet const&);

// Which of the given permissions permissions.remove() takes away: only optional ones that are granted. Removing a
// required one, or one the manifest doesn't declare, fails with the error extensions compare.
WEBVIEW_API ErrorOr<ExtensionPermissionSet, String> extension_permissions_to_remove(Extension const&, ExtensionPermissionSet const&);

// The optional permissions among the given ones that a grant adds, leaving out what's undeclared or already granted.
WEBVIEW_API ExtensionPermissionSet extension_permissions_to_grant(Extension const&, ExtensionPermissionSet const&);

}
