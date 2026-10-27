/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>

namespace Web::Extensions {

// What scripting.executeScript() or tabs.executeScript() runs in one document: the sources of files, or a function's
// source with its arguments as a JSON array, in the extension's world or the page's, once the document reaches the
// run_at point (or at once, with none). An insertCSS() call adds style sheets instead, each with a key naming its file
// or its code, and a removeCSS() call takes away the extension's sheets with the same keys and origin.
struct ExtensionInjection {
    Vector<String> js_paths;
    Vector<String> js_sources;
    Optional<String> function_source;
    String arguments_json;
    ExecutionWorld world { ExecutionWorld::Isolated };
    Vector<String> css_sources;
    Vector<String> css_keys;
    bool css_user_origin { false };
    bool remove_css { false };
    Optional<RunAt> run_at;
    bool operator==(ExtensionInjection const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::Extensions::ExtensionInjection const&);
template<>
WEBCOMMON_API ErrorOr<Web::Extensions::ExtensionInjection> decode(Decoder&);

}
