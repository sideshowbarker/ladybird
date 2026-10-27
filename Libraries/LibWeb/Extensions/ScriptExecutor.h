/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/Injection.h>

namespace Web::Extensions {

enum class FrameRemoved : bool {
    No,
    Yes,
};

// Runs an injection in the document, in the extension's world or the page's, once the document reaches the injection's
// run_at point, and calls back once: with the result as JSON (none when it's undefined, null when JSON can't hold it),
// with the error it threw, or with FrameRemoved::Yes if the document goes away first. A promise is waited for.
using InjectionCallback = Function<void(Optional<String> result_json, Optional<String> error, FrameRemoved)>;
WEB_API void execute_injection(DOM::Document&, ExtensionDescriptor const&, ExtensionInjection const&, InjectionCallback);

// Runs the injections waiting for the document to reach a run_at point, once it has.
void run_pending_injections(DOM::Document&, RunAt reached);
// Calls back every injection that hasn't finished in the document, which is going away.
void cancel_pending_injections(DOM::Document&);

}
