/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/StringView.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebView/Forward.h>

namespace WebView {

class ExtensionActionRegistry;

// Whether a context may call the action API: only the extension's own documents, and only in the namespace its
// manifest version has (action in version 3, browserAction in version 2) when its manifest declares an action.
WEBVIEW_API bool may_call_extension_action_api(bool is_extension_document, StringView api_namespace, Web::Extensions::ExtensionDescriptor const&);

// Runs one call to the action API ("setTitle", e.g., without its namespace), whose arguments are a tab id or null,
// then the value a setter sets. Returns what the call resolves with, or the message it rejects with.
WEBVIEW_API ErrorOr<JsonValue, String> call_extension_action_api(ExtensionActionRegistry&, Web::Extensions::ExtensionDescriptor const&, StringView method, JsonArray const& arguments);

}
