/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// Runs the content scripts of every enabled extension that match the document, for each run_at point up to and
// including the given one that the document hasn't reached yet. The document calls this when its document element
// is created (document_start), after it fires DOMContentLoaded (document_end), and when it is idle: 200 ms after
// DOMContentLoaded or when it completely finishes loading, whichever comes first (document_idle).
WEB_API void run_content_scripts(DOM::Document&, RunAt up_to);

// Whether a content script's frame and URL rules select the document.
WEB_API bool content_script_matches(DOM::Document const&, ContentScriptDescriptor const&);

}
