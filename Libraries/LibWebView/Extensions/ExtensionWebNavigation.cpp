/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/Extensions/ExtensionTabs.h>
#include <LibWebView/Extensions/ExtensionWebNavigation.h>

namespace WebView {

WebNavigationFrameIds web_navigation_frame_ids(CanonicalNavigable const& navigable)
{
    auto const* parent = navigable.parent();
    if (!parent)
        return {};
    return { extension_frame_id_for(navigable.id()), parent->parent() ? extension_frame_id_for(parent->id()) : 0 };
}

static JsonObject frame_json(CanonicalNavigable const& navigable)
{
    auto ids = web_navigation_frame_ids(navigable);
    JsonObject frame;
    // The error page shown in place of a failed navigation's document.
    frame.set("errorOccurred"sv, navigable.active_document().creation_url() == URL::about_error());
    frame.set("url"sv, navigable.active_document_url().serialize());
    frame.set("frameId"sv, ids.frame_id);
    frame.set("parentFrameId"sv, ids.parent_frame_id);
    return frame;
}

JsonValue web_navigation_frame(CanonicalTraversable const& traversable, i64 frame_id)
{
    JsonValue result;
    traversable.for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        if (web_navigation_frame_ids(navigable).frame_id != frame_id)
            return IterationDecision::Continue;
        auto frame = frame_json(navigable);
        frame.remove("frameId"sv);
        result = move(frame);
        return IterationDecision::Break;
    });
    return result;
}

JsonArray web_navigation_all_frames(CanonicalTraversable const& traversable)
{
    JsonArray frames;
    traversable.for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        frames.must_append(frame_json(navigable));
        return IterationDecision::Continue;
    });
    return frames;
}

}
