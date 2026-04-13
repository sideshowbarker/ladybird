/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdAtkObject.h"

#include <AK/HashMap.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibUnicode/Segmenter.h>

// Known limitation: AtkText::get_character_extents computes per-character x-coordinates using the node's font
// metrics along a single horizontal line. This is correct for non-wrapping text but falls back to proportional
// slicing when text wraps onto multiple lines, since AccessibilityNodeData carries only the node's total bounding
// rect and not per-line fragment geometry. A fully correct implementation would require integrating with Layout's
// Fragment data, which is intentionally kept out of scope for the accessibility layer.

namespace Ladybird {

// --- Object struct ---

struct LadybirdAtkObjectData {
    AtkObject parent;
    i64 node_id;
    WebView::AccessibilityTreeManager const* manager;
    AccessibilityActionCallback* action_callback;
    AccessibilityTextActionCallback* text_action_callback;

    // Caches for strings returned as char const* to ATK accessors. Each is g_strdup'd on read and
    // g_free'd in finalize. Per-instance (rather than per-thread) so that a caller holding the
    // result of atk_object_get_name(objA) doesn't see it dangle after an atk_object_get_name(objB)
    // elsewhere on the same thread.
    gchar* cached_name;
    gchar* cached_description;
    gchar* cached_keybinding;
    gchar* cached_locale;
    gchar* cached_image_description;
    gchar* cached_image_locale;
    gchar* cached_summary;

    // Cache of build_hypertext() result. Many AtkText accessors (get_text, get_character_count,
    // get_character_at_offset, get_string_at_offset, etc.) call build_hypertext on every invocation.
    // A single Orca command frequently invokes several of those in a row against the same node; the
    // cache avoids rebuilding the same hypertext string repeatedly. The generation counter matches
    // AccessibilityTreeManager::generation() at the time the hypertext was built; when the tree is
    // replaced, the generation advances and the cache is discarded on next read.
    gchar* cached_hypertext;
    size_t cached_hypertext_length;
    u64 cached_hypertext_generation;
};

struct LadybirdAtkObjectDataClass {
    AtkObjectClass parent_class;
};

using LadybirdAtkObject = LadybirdAtkObjectData;
using LadybirdAtkObjectClass = LadybirdAtkObjectDataClass;

// Forward declarations needed for G_DEFINE_TYPE_WITH_CODE and functions that reference each other.
static void ladybird_text_iface_init(AtkTextIface*);
static void ladybird_component_iface_init(AtkComponentIface*);
static void ladybird_action_iface_init(AtkActionIface*);
static void ladybird_document_iface_init(AtkDocumentIface*);
static void ladybird_hypertext_iface_init(AtkHypertextIface*);
static void ladybird_image_iface_init(AtkImageIface*);
static void ladybird_editable_text_iface_init(AtkEditableTextIface*);
static void ladybird_value_iface_init(AtkValueIface*);
static void ladybird_selection_iface_init(AtkSelectionIface*);
static void ladybird_table_iface_init(AtkTableIface*);
static void ladybird_table_cell_iface_init(AtkTableCellIface*);

static WebView::AccessibilityNodeData const* get_node_data(LadybirdAtkObject* self)
{
    if (!self->manager)
        return nullptr;
    return self->manager->node(self->node_id);
}

static HashMap<i64, AtkObject*> s_atk_object_cache;

// --- Helpers ---

static bool is_ignored_role(StringView role, String const& name)
{
    if ((role == "generic"sv || role == "paragraph"sv) && name.is_empty())
        return true;
    return false;
}

static void collect_unignored_children(WebView::AccessibilityTreeManager const* manager, i64 node_id, Vector<i64>& out)
{
    auto const* data = manager->node(node_id);
    if (!data)
        return;
    for (auto child_id : data->child_ids) {
        auto const* child_data = manager->node(child_id);
        if (!child_data)
            continue;
        if (is_ignored_role(child_data->role.bytes_as_string_view(), child_data->name))
            collect_unignored_children(manager, child_id, out);
        else
            out.append(child_id);
    }
}

// Build hypertext for a node: text-leaf children contribute their text, non-text children contribute U+FFFC.
static ByteString build_hypertext(WebView::AccessibilityTreeManager const* manager, i64 node_id)
{
    auto const* data = manager->node(node_id);
    if (!data)
        return {};
    if (data->role.bytes_as_string_view() == "text leaf"sv)
        return data->name.to_byte_string();

    Vector<i64> children;
    collect_unignored_children(manager, node_id, children);
    if (children.is_empty())
        return data->name.to_byte_string();

    StringBuilder builder;
    for (auto child_id : children) {
        auto const* child_data = manager->node(child_id);
        if (!child_data)
            continue;
        if (child_data->role.bytes_as_string_view() == "text leaf"sv) {
            builder.append(child_data->name.to_byte_string());
        } else {
            // U+FFFC (object replacement character) in UTF-8.
            builder.append('\xEF');
            builder.append('\xBF');
            builder.append('\xBC');
        }
    }
    return builder.to_byte_string();
}

// --- AtkText interface ---

// Return a StringView into the per-instance hypertext cache on `self`, rebuilding it if the tree has
// advanced since we last cached. The returned view is valid until the next cached_hypertext_view call
// on the same object.
static StringView cached_hypertext_view(LadybirdAtkObject* self)
{
    if (!self || !self->manager)
        return {};
    auto current_generation = self->manager->generation();
    if (self->cached_hypertext && self->cached_hypertext_generation == current_generation)
        return { self->cached_hypertext, self->cached_hypertext_length };
    auto text = build_hypertext(self->manager, self->node_id);
    g_free(self->cached_hypertext);
    self->cached_hypertext = g_strdup(text.characters());
    self->cached_hypertext_length = text.length();
    self->cached_hypertext_generation = current_generation;
    return { self->cached_hypertext, self->cached_hypertext_length };
}

// ATK text offsets are Unicode character (codepoint) offsets, not byte offsets.

static gchar* ladybird_text_get_text(AtkText* text, gint start, gint end)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto full = cached_hypertext_view(self);
    if (full.is_empty() || start < 0)
        return g_strdup("");

    Utf8View view { full };
    auto character_count = static_cast<gint>(view.length());
    if (end < 0 || end > character_count)
        end = character_count;
    if (start >= end)
        return g_strdup("");

    auto byte_start = view.byte_offset_of(static_cast<size_t>(start));
    auto byte_end = view.byte_offset_of(static_cast<size_t>(end));
    return g_strndup(full.characters_without_null_termination() + byte_start, byte_end - byte_start);
}

static gint ladybird_text_get_character_count(AtkText* text)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto full = cached_hypertext_view(self);
    return static_cast<gint>(Utf8View(full).length());
}

static gunichar ladybird_text_get_character_at_offset(AtkText* text, gint offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto full = cached_hypertext_view(self);
    if (offset < 0)
        return 0;
    size_t characters_seen = 0;
    for (u32 codepoint : Utf8View(full)) {
        if (characters_seen == static_cast<size_t>(offset))
            return static_cast<gunichar>(codepoint);
        ++characters_seen;
    }
    return 0;
}

// Helper to prepend an AttributeSet entry; returns the new head of the list. Callers must g_strdup strings.
static AtkAttributeSet* prepend_attribute(AtkAttributeSet* set, char const* name, char const* value)
{
    auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
    attr->name = g_strdup(name);
    attr->value = g_strdup(value);
    return g_slist_prepend(set, attr);
}

static AtkAttributeSet* build_text_attributes(WebView::AccessibilityNodeData const* data)
{
    AtkAttributeSet* attrs = prepend_attribute(nullptr, "direction", "ltr");
    if (!data)
        return attrs;
    if (!data->font_family.is_empty())
        attrs = prepend_attribute(attrs, "family-name", data->font_family.to_byte_string().characters());
    if (!data->font_size.is_empty())
        attrs = prepend_attribute(attrs, "size", data->font_size.to_byte_string().characters());
    if (!data->font_weight.is_empty())
        attrs = prepend_attribute(attrs, "weight", data->font_weight.to_byte_string().characters());
    if (!data->font_style.is_empty())
        attrs = prepend_attribute(attrs, "style", data->font_style.to_byte_string().characters());
    if (!data->color.is_empty())
        attrs = prepend_attribute(attrs, "fg-color", data->color.to_byte_string().characters());
    if (!data->background_color.is_empty())
        attrs = prepend_attribute(attrs, "bg-color", data->background_color.to_byte_string().characters());
    if (!data->text_decoration.is_empty()) {
        // AT-SPI2 exposes text-decoration as separate attributes: "underline" takes "none"/"single"/"double", and
        // "strikethrough" takes "true"/"false". Translate from our space-separated CSS decoration list.
        auto decoration = data->text_decoration.bytes_as_string_view();
        attrs = prepend_attribute(attrs, "underline", decoration.contains("underline"sv) ? "single" : "none");
        attrs = prepend_attribute(attrs, "strikethrough",
            decoration.contains("line-through"sv) ? "true" : "false");
    }
    return attrs;
}

static AtkAttributeSet* ladybird_text_get_run_attributes(AtkText* text, gint, gint* start, gint* end)
{
    // AccessibilityNodeData stores formatting per text-leaf node (inherited from the parent element's computed
    // style). Since a single text leaf has uniform formatting, the run spans the entire node text.
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto full = cached_hypertext_view(self);
    *start = 0;
    *end = static_cast<gint>(Utf8View(full).length());
    return build_text_attributes(get_node_data(self));
}

static AtkAttributeSet* ladybird_text_get_default_attributes(AtkText* text)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    return build_text_attributes(get_node_data(self));
}

// Helpers for text boundary navigation (CHAR/WORD/SENTENCE granularities). Uses Unicode::Segmenter to find grapheme,
// word, and sentence boundaries in the hypertext string. All offsets are Unicode character (codepoint) offsets.

static size_t character_offset_to_byte_offset(StringView utf8, size_t character_offset)
{
    Utf8View view { utf8 };
    return view.byte_offset_of(character_offset);
}

static size_t byte_offset_to_character_offset(StringView utf8, size_t byte_offset)
{
    Utf8View view { utf8 };
    return view.code_point_offset_of(byte_offset);
}

static gchar* text_at_boundary(LadybirdAtkObject* self, gint offset, AtkTextGranularity granularity, gint* start_out,
    gint* end_out)
{
    auto full = cached_hypertext_view(self);
    Utf8View view { full };
    auto character_count = static_cast<gint>(view.length());

    if (offset < 0 || offset >= character_count) {
        *start_out = 0;
        *end_out = 0;
        return g_strdup("");
    }

    if (granularity == ATK_TEXT_GRANULARITY_CHAR) {
        *start_out = offset;
        *end_out = offset + 1;
        auto byte_start = view.byte_offset_of(static_cast<size_t>(offset));
        auto byte_end = view.byte_offset_of(static_cast<size_t>(offset) + 1);
        return g_strndup(full.characters_without_null_termination() + byte_start, byte_end - byte_start);
    }

    auto segmenter_granularity = Unicode::SegmenterGranularity::Grapheme;
    switch (granularity) {
    case ATK_TEXT_GRANULARITY_WORD:
        segmenter_granularity = Unicode::SegmenterGranularity::Word;
        break;
    case ATK_TEXT_GRANULARITY_SENTENCE:
        segmenter_granularity = Unicode::SegmenterGranularity::Sentence;
        break;
    case ATK_TEXT_GRANULARITY_LINE:
    case ATK_TEXT_GRANULARITY_PARAGRAPH: {
        // Use Layout fragment data (line_break_character_offsets) for correct line boundaries.
        auto const* data = get_node_data(self);
        if (data && !data->line_break_character_offsets.is_empty()) {
            gint line_start = 0;
            gint line_end = character_count;
            for (auto break_offset : data->line_break_character_offsets) {
                if (break_offset <= offset)
                    line_start = break_offset;
                if (break_offset > offset) {
                    line_end = break_offset;
                    break;
                }
            }
            *start_out = line_start;
            *end_out = line_end;
            auto byte_start = view.byte_offset_of(static_cast<size_t>(line_start));
            auto byte_end = view.byte_offset_of(static_cast<size_t>(line_end));
            return g_strndup(full.characters_without_null_termination() + byte_start, byte_end - byte_start);
        }
        // Layout fragment data unavailable (e.g. pre-layout). Fall back to sentence granularity so Orca can still
        // make forward progress.
        segmenter_granularity = Unicode::SegmenterGranularity::Sentence;
        break;
    }
    default:
        segmenter_granularity = Unicode::SegmenterGranularity::Word;
        break;
    }

    auto segmenter = Unicode::Segmenter::create(segmenter_granularity);
    auto full_string = String::from_utf8(full).release_value_but_fixme_should_propagate_errors();
    segmenter->set_segmented_text(full_string);

    // Segmenter operates on UTF-8 byte offsets when given a String.
    auto byte_offset = character_offset_to_byte_offset(full, static_cast<size_t>(offset));
    auto previous_byte = segmenter->previous_boundary(byte_offset, Unicode::Segmenter::Inclusive::Yes).value_or(0);
    auto next_byte = segmenter->next_boundary(byte_offset).value_or(full.length());

    *start_out = static_cast<gint>(byte_offset_to_character_offset(full, previous_byte));
    *end_out = static_cast<gint>(byte_offset_to_character_offset(full, next_byte));
    return g_strndup(full.characters_without_null_termination() + previous_byte, next_byte - previous_byte);
}

static gchar* ladybird_text_get_string_at_offset(AtkText* text, gint offset, AtkTextGranularity granularity,
    gint* start_offset, gint* end_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    return text_at_boundary(self, offset, granularity, start_offset, end_offset);
}

// Legacy text boundary API (deprecated in ATK but still used by older Orca versions). Map the legacy boundary
// types to the modern granularity enum and delegate to text_at_boundary.
static AtkTextGranularity boundary_to_granularity(AtkTextBoundary boundary)
{
    switch (boundary) {
    case ATK_TEXT_BOUNDARY_CHAR:
        return ATK_TEXT_GRANULARITY_CHAR;
    case ATK_TEXT_BOUNDARY_WORD_START:
    case ATK_TEXT_BOUNDARY_WORD_END:
        return ATK_TEXT_GRANULARITY_WORD;
    case ATK_TEXT_BOUNDARY_SENTENCE_START:
    case ATK_TEXT_BOUNDARY_SENTENCE_END:
        return ATK_TEXT_GRANULARITY_SENTENCE;
    case ATK_TEXT_BOUNDARY_LINE_START:
    case ATK_TEXT_BOUNDARY_LINE_END:
        return ATK_TEXT_GRANULARITY_LINE;
    default:
        return ATK_TEXT_GRANULARITY_WORD;
    }
}

static gchar* ladybird_text_get_text_at_offset(AtkText* text, gint offset, AtkTextBoundary boundary,
    gint* start_offset, gint* end_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    return text_at_boundary(self, offset, boundary_to_granularity(boundary), start_offset, end_offset);
}

static gchar* ladybird_text_get_text_before_offset(AtkText* text, gint offset, AtkTextBoundary boundary,
    gint* start_offset, gint* end_offset)
{
    // Get the boundary at offset, then return the segment ending at its start.
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    gint current_start = 0;
    gint current_end = 0;
    auto* current = text_at_boundary(self, offset, boundary_to_granularity(boundary), &current_start, &current_end);
    g_free(current);
    if (current_start <= 0) {
        *start_offset = 0;
        *end_offset = 0;
        return g_strdup("");
    }
    return text_at_boundary(self, current_start - 1, boundary_to_granularity(boundary), start_offset, end_offset);
}

static gchar* ladybird_text_get_text_after_offset(AtkText* text, gint offset, AtkTextBoundary boundary,
    gint* start_offset, gint* end_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    gint current_start = 0;
    gint current_end = 0;
    auto* current = text_at_boundary(self, offset, boundary_to_granularity(boundary), &current_start, &current_end);
    g_free(current);
    auto full = cached_hypertext_view(self);
    Utf8View view { full };
    auto character_count = static_cast<gint>(view.length());
    if (current_end >= character_count) {
        *start_offset = character_count;
        *end_offset = character_count;
        return g_strdup("");
    }
    return text_at_boundary(self, current_end, boundary_to_granularity(boundary), start_offset, end_offset);
}

static gint ladybird_text_get_offset_at_point(AtkText* text, gint x, gint y, AtkCoordType)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(text));
    if (!data || data->character_offsets.is_empty())
        return -1;
    // Query coordinates are screen-space; convert to text-local coordinates.
    auto relative_x = x - data->bounds.x();
    auto relative_y = y - data->bounds.y();
    // Find the character whose (x, y) is closest to the query point (on the nearest line).
    auto const& offsets = data->character_offsets;
    size_t best_index = 0;
    long long best_distance = -1;
    for (size_t i = 0; i < offsets.size(); ++i) {
        long long dx = offsets[i].x() - relative_x;
        long long dy = offsets[i].y() - relative_y;
        long long distance_squared = dx * dx + dy * dy;
        if (best_distance < 0 || distance_squared < best_distance) {
            best_distance = distance_squared;
            best_index = i;
        }
    }
    return static_cast<gint>(best_index);
}

static void ladybird_text_get_range_extents(AtkText* text, gint start_offset, gint end_offset, AtkCoordType,
    AtkTextRectangle* rect)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(text));
    if (!data || !rect)
        return;
    if (data->character_offsets.is_empty() || start_offset < 0 || end_offset <= start_offset) {
        rect->x = data->bounds.x();
        rect->y = data->bounds.y();
        rect->width = 0;
        rect->height = data->bounds.height();
        return;
    }
    auto count = static_cast<gint>(data->character_offsets.size());
    auto clamped_start = min(start_offset, count - 1);
    auto clamped_end = min(end_offset, count);
    // Compute the union of per-character rects in the range. For single-line ranges this is just the extent from
    // start to end; for multi-line ranges it spans multiple lines and the result is the bounding rect of all
    // characters in the range.
    auto first_x = data->character_offsets[clamped_start].x();
    auto first_y = data->character_offsets[clamped_start].y();
    gint max_right = first_x;
    gint max_bottom = first_y;
    for (gint i = clamped_start; i < clamped_end; ++i) {
        auto const& p = data->character_offsets[i];
        max_right = max(max_right, p.x());
        max_bottom = max(max_bottom, p.y());
    }
    rect->x = data->bounds.x() + first_x;
    rect->y = data->bounds.y() + first_y;
    rect->width = max_right - first_x + 1;
    rect->height = max_bottom - first_y + (data->line_heights.is_empty() ? data->bounds.height() : data->line_heights[0]);
}

static void ladybird_text_get_character_extents(AtkText* text, gint offset, gint* x, gint* y, gint* width,
    gint* height, AtkCoordType)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(text));
    if (!data) {
        *x = *y = *width = *height = 0;
        return;
    }

    // Use per-character offsets (now a full Vector<Gfx::IntPoint>). Entry[i] gives (x, y) of character i relative
    // to the text's bounds origin.
    if (!data->character_offsets.is_empty() && offset >= 0
        && static_cast<size_t>(offset) < data->character_offsets.size()) {
        auto const& p = data->character_offsets[offset];
        *x = data->bounds.x() + p.x();
        *y = data->bounds.y() + p.y();
        // Height is the per-line height if we have it, otherwise full bounds height.
        *height = data->line_heights.is_empty() ? data->bounds.height() : data->line_heights[0];
        // Width: next character's x (if on same line) or line end.
        if (static_cast<size_t>(offset) + 1 < data->character_offsets.size()) {
            auto const& next = data->character_offsets[offset + 1];
            if (next.y() == p.y())
                *width = next.x() - p.x();
            else
                *width = data->bounds.width() - p.x(); // End of line — no good way to know line end x.
        } else {
            *width = data->bounds.width() - p.x();
        }
        return;
    }

    // No per-character offsets: fall back to the whole node's bounds.
    *x = data->bounds.x();
    *y = data->bounds.y();
    *width = data->bounds.width();
    *height = data->bounds.height();
}

static gint ladybird_text_get_caret_offset(AtkText* text)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto const* data = get_node_data(self);
    if (!data)
        return -1;
    // caret_offset is populated from DOM Selection when the node contains the caret; defaults to -1 otherwise.
    return data->caret_offset;
}

static gboolean ladybird_text_set_caret_offset(AtkText* text, gint offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    if (self->text_action_callback) {
        (*self->text_action_callback)(self->node_id, "set_caret_offset"_string, offset, offset, String {});
        return TRUE;
    }
    // Fallback: move focus to the node if the text-action callback isn't wired up.
    if (self->action_callback) {
        (*self->action_callback)(self->node_id, "focus"_string);
        return TRUE;
    }
    return FALSE;
}

static gint ladybird_text_get_n_selections(AtkText* text)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto const* data = get_node_data(self);
    if (!data || data->selection_start < 0 || data->selection_end < 0)
        return 0;
    return 1;
}

static gchar* ladybird_text_get_selection(AtkText* text, gint selection_num, gint* start_offset, gint* end_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    auto const* data = get_node_data(self);
    if (!data || selection_num != 0 || data->selection_start < 0 || data->selection_end < 0) {
        *start_offset = 0;
        *end_offset = 0;
        return nullptr;
    }
    *start_offset = data->selection_start;
    *end_offset = data->selection_end;
    return ladybird_text_get_text(text, *start_offset, *end_offset);
}

static gboolean ladybird_text_add_selection(AtkText* text, gint start_offset, gint end_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    if (!self->text_action_callback)
        return FALSE;
    (*self->text_action_callback)(self->node_id, "set_selection"_string, start_offset, end_offset, String {});
    return TRUE;
}

static gboolean ladybird_text_remove_selection(AtkText* text, gint selection_num)
{
    if (selection_num != 0)
        return FALSE;
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    if (!self->text_action_callback)
        return FALSE;
    // Removing a selection means collapsing it to a caret at the current selection start.
    auto const* data = get_node_data(self);
    auto offset = (data && data->selection_start >= 0) ? data->selection_start : 0;
    (*self->text_action_callback)(self->node_id, "set_caret_offset"_string, offset, offset, String {});
    return TRUE;
}

static gboolean ladybird_text_set_selection(AtkText* text, gint selection_num, gint start_offset, gint end_offset)
{
    if (selection_num != 0)
        return FALSE;
    return ladybird_text_add_selection(text, start_offset, end_offset);
}

static gboolean ladybird_text_scroll_substring_to(AtkText* text, gint start_offset, gint end_offset, AtkScrollType)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(text);
    if (!self->text_action_callback)
        return FALSE;
    (*self->text_action_callback)(self->node_id, "scroll_substring_to"_string, start_offset, end_offset, String {});
    return TRUE;
}

static gboolean ladybird_text_scroll_substring_to_point(AtkText* text, gint start_offset, gint end_offset,
    AtkCoordType, gint, gint)
{
    // We don't honor the explicit target point; fall back to scrolling into view.
    return ladybird_text_scroll_substring_to(text, start_offset, end_offset, ATK_SCROLL_ANYWHERE);
}

static void ladybird_text_iface_init(AtkTextIface* iface)
{
    iface->get_text = ladybird_text_get_text;
    iface->get_character_count = ladybird_text_get_character_count;
    iface->get_character_at_offset = ladybird_text_get_character_at_offset;
    iface->get_run_attributes = ladybird_text_get_run_attributes;
    iface->get_default_attributes = ladybird_text_get_default_attributes;
    iface->get_character_extents = ladybird_text_get_character_extents;
    iface->get_offset_at_point = ladybird_text_get_offset_at_point;
    iface->get_range_extents = ladybird_text_get_range_extents;
    iface->get_string_at_offset = ladybird_text_get_string_at_offset;
    iface->get_text_at_offset = ladybird_text_get_text_at_offset;
    iface->get_text_before_offset = ladybird_text_get_text_before_offset;
    iface->get_text_after_offset = ladybird_text_get_text_after_offset;
    iface->get_caret_offset = ladybird_text_get_caret_offset;
    iface->set_caret_offset = ladybird_text_set_caret_offset;
    iface->get_n_selections = ladybird_text_get_n_selections;
    iface->get_selection = ladybird_text_get_selection;
    iface->add_selection = ladybird_text_add_selection;
    iface->remove_selection = ladybird_text_remove_selection;
    iface->set_selection = ladybird_text_set_selection;
    iface->scroll_substring_to = ladybird_text_scroll_substring_to;
    iface->scroll_substring_to_point = ladybird_text_scroll_substring_to_point;
}

// --- AtkComponent interface ---

static void ladybird_component_get_extents(AtkComponent* component, gint* x, gint* y, gint* width, gint* height,
    AtkCoordType)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(component));
    if (data) {
        *x = data->bounds.x();
        *y = data->bounds.y();
        *width = data->bounds.width();
        *height = data->bounds.height();
    }
}

static void ladybird_component_iface_init(AtkComponentIface* iface)
{
    iface->get_extents = ladybird_component_get_extents;
}

// --- AtkAction interface ---

static gint ladybird_action_get_n_actions(AtkAction*)
{
    return 2; // Press and focus.
}

static char const* ladybird_action_get_name(AtkAction* action, gint index)
{
    if (index == 1)
        return "focus";
    if (index != 0)
        return nullptr;

    // The first action's name varies by element role. Orca reads this to announce how to interact.
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(action));
    if (!data)
        return "click";

    auto role = data->role.bytes_as_string_view();
    if (role == "link"sv)
        return "jump";
    if (role == "checkbox"sv || role == "radio"sv || role == "menuitemcheckbox"sv || role == "menuitemradio"sv)
        return "toggle";
    if (role == "button"sv && data->is_pressed)
        return "toggle";
    if (role == "option"sv || role == "tab"sv || role == "menuitem"sv)
        return "select";
    return "click";
}

static gboolean ladybird_action_do_action(AtkAction* action, gint index)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(action);
    if (!self->action_callback)
        return FALSE;

    // WebContent accepts "press"/"click" and "focus" as action strings. Role-specific names reported by
    // get_name (jump, toggle, select) still map to a click, since clicking is what drives all of them on a
    // rendered HTML element.
    if (index == 0)
        (*self->action_callback)(self->node_id, "press"_string);
    else if (index == 1)
        (*self->action_callback)(self->node_id, "focus"_string);
    else
        return FALSE;
    return TRUE;
}

static char const* ladybird_action_get_description(AtkAction*, gint index)
{
    if (index == 0)
        return "Activate the element, as if clicking it";
    if (index == 1)
        return "Move focus to the element";
    return nullptr;
}

static char const* ladybird_action_get_localized_name(AtkAction* action, gint index)
{
    // We don't have a localization framework here; return the canonical name as the localized name.
    return ladybird_action_get_name(action, index);
}

static char const* ladybird_action_get_keybinding(AtkAction* action, gint index)
{
    if (index != 0)
        return nullptr;
    auto* self = reinterpret_cast<LadybirdAtkObject*>(action);
    auto const* data = get_node_data(self);
    if (!data || data->keybinding.is_empty())
        return nullptr;
    g_free(self->cached_keybinding);
    self->cached_keybinding = g_strdup(data->keybinding.to_byte_string().characters());
    return self->cached_keybinding;
}

static void ladybird_action_iface_init(AtkActionIface* iface)
{
    iface->get_n_actions = ladybird_action_get_n_actions;
    iface->get_name = ladybird_action_get_name;
    iface->get_description = ladybird_action_get_description;
    iface->get_localized_name = ladybird_action_get_localized_name;
    iface->get_keybinding = ladybird_action_get_keybinding;
    iface->do_action = ladybird_action_do_action;
}

// --- AtkDocument interface ---

static char const* ladybird_document_get_locale(AtkDocument* document)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(document);
    auto const* data = get_node_data(self);
    if (!data || data->language.is_empty())
        return "";
    g_free(self->cached_locale);
    self->cached_locale = g_strdup(data->language.to_byte_string().characters());
    return self->cached_locale;
}

static char const* ladybird_document_get_document_type(AtkDocument*)
{
    return "text/html";
}

static AtkAttributeSet* ladybird_document_get_attributes(AtkDocument*)
{
    AtkAttributeSet* attrs = nullptr;
    auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
    attr->name = g_strdup("DocType");
    attr->value = g_strdup("text/html");
    attrs = g_slist_prepend(attrs, attr);
    attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
    attr->name = g_strdup("MimeType");
    attr->value = g_strdup("text/html");
    attrs = g_slist_prepend(attrs, attr);
    return attrs;
}

static void ladybird_document_iface_init(AtkDocumentIface* iface)
{
    iface->get_document_locale = ladybird_document_get_locale;
    iface->get_document_type = ladybird_document_get_document_type;
    iface->get_document_attributes = ladybird_document_get_attributes;
}

// --- AtkHyperlink subclass ---

// Each embedded (non-text-leaf) child in the hypertext stream is represented by an AtkHyperlink. This wraps the child
// AtkObject and reports its position within the parent's hypertext string (where it occupies a single U+FFFC object
// replacement character, or 3 bytes in UTF-8).

struct LadybirdAtkHyperlink {
    AtkHyperlink parent;
    i64 parent_node_id;
    i64 child_node_id;
    WebView::AccessibilityTreeManager const* manager;
};

struct LadybirdAtkHyperlinkClass {
    AtkHyperlinkClass parent_class;
};

GType ladybird_atk_hyperlink_get_type();
G_DEFINE_TYPE(LadybirdAtkHyperlink, ladybird_atk_hyperlink, ATK_TYPE_HYPERLINK)

// Compute the character (not byte) offset within the parent's hypertext string where the given child's U+FFFC
// character appears. Returns -1 if the child is not found among the parent's unignored non-text-leaf children.
static gint hyperlink_offset_in_parent(WebView::AccessibilityTreeManager const* manager, i64 parent_node_id,
    i64 child_node_id)
{
    Vector<i64> children;
    collect_unignored_children(manager, parent_node_id, children);

    gint offset = 0;
    for (auto current_child_id : children) {
        auto const* current_child_data = manager->node(current_child_id);
        if (!current_child_data)
            continue;
        if (current_child_data->role.bytes_as_string_view() == "text leaf"sv) {
            offset += static_cast<gint>(Utf8View(current_child_data->name.bytes_as_string_view()).length());
        } else {
            if (current_child_id == child_node_id)
                return offset;
            // U+FFFC is a single Unicode character.
            offset += 1;
        }
    }
    return -1;
}

static gchar* ladybird_atk_hyperlink_get_uri(AtkHyperlink* hyperlink, gint index)
{
    if (index != 0)
        return nullptr;
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    auto const* data = self->manager ? self->manager->node(self->child_node_id) : nullptr;
    if (!data || data->url.is_empty())
        return nullptr;
    return g_strdup(data->url.to_byte_string().characters());
}

static AtkObject* ladybird_atk_hyperlink_get_object(AtkHyperlink* hyperlink, gint index)
{
    if (index != 0)
        return nullptr;
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    return ladybird_atk_object_new(self->child_node_id, self->manager);
}

static gint ladybird_atk_hyperlink_get_n_anchors(AtkHyperlink* hyperlink)
{
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    return (self->manager && self->manager->node(self->child_node_id)) ? 1 : 0;
}

static gint ladybird_atk_hyperlink_get_start_index(AtkHyperlink* hyperlink)
{
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    auto offset = hyperlink_offset_in_parent(self->manager, self->parent_node_id, self->child_node_id);
    return offset < 0 ? 0 : offset;
}

static gint ladybird_atk_hyperlink_get_end_index(AtkHyperlink* hyperlink)
{
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    auto offset = hyperlink_offset_in_parent(self->manager, self->parent_node_id, self->child_node_id);
    if (offset < 0)
        return 0;
    // The hyperlink occupies one U+FFFC character in the parent's hypertext stream.
    return offset + 1;
}

static gboolean ladybird_atk_hyperlink_is_valid(AtkHyperlink* hyperlink)
{
    auto* self = reinterpret_cast<LadybirdAtkHyperlink*>(hyperlink);
    return self->manager && self->manager->node(self->child_node_id) ? TRUE : FALSE;
}

static void ladybird_atk_hyperlink_init(LadybirdAtkHyperlink* self)
{
    self->parent_node_id = -1;
    self->child_node_id = -1;
    self->manager = nullptr;
}

static void ladybird_atk_hyperlink_class_init(LadybirdAtkHyperlinkClass* klass)
{
    auto* hyperlink_class = ATK_HYPERLINK_CLASS(klass);
    hyperlink_class->get_uri = ladybird_atk_hyperlink_get_uri;
    hyperlink_class->get_object = ladybird_atk_hyperlink_get_object;
    hyperlink_class->get_n_anchors = ladybird_atk_hyperlink_get_n_anchors;
    hyperlink_class->get_start_index = ladybird_atk_hyperlink_get_start_index;
    hyperlink_class->get_end_index = ladybird_atk_hyperlink_get_end_index;
    hyperlink_class->is_valid = ladybird_atk_hyperlink_is_valid;
}

// Cache of AtkHyperlink instances keyed by the child node ID they wrap.
static HashMap<i64, AtkHyperlink*> s_atk_hyperlink_cache;

static AtkHyperlink* get_or_create_hyperlink(WebView::AccessibilityTreeManager const* manager, i64 parent_node_id,
    i64 child_node_id)
{
    if (auto it = s_atk_hyperlink_cache.find(child_node_id); it != s_atk_hyperlink_cache.end())
        return it->value;

    auto* hyperlink = static_cast<LadybirdAtkHyperlink*>(g_object_new(ladybird_atk_hyperlink_get_type(), nullptr));
    hyperlink->parent_node_id = parent_node_id;
    hyperlink->child_node_id = child_node_id;
    hyperlink->manager = manager;
    auto* atk_hyperlink = reinterpret_cast<AtkHyperlink*>(hyperlink);
    s_atk_hyperlink_cache.set(child_node_id, atk_hyperlink);
    return atk_hyperlink;
}

// --- AtkHypertext interface ---

// In hypertext terms, "links" are non-text-leaf children embedded in the container's text stream.
static gint ladybird_hypertext_get_n_links(AtkHypertext* hypertext)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(hypertext);
    auto const* data = self->manager->node(self->node_id);
    if (!data)
        return 0;

    gint count = 0;
    Vector<i64> children;
    collect_unignored_children(self->manager, self->node_id, children);
    for (auto child_id : children) {
        auto const* child_data = self->manager->node(child_id);
        if (child_data && child_data->role.bytes_as_string_view() != "text leaf"sv)
            count++;
    }
    return count;
}

static AtkHyperlink* ladybird_hypertext_get_link(AtkHypertext* hypertext, gint link_index)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(hypertext);
    if (link_index < 0)
        return nullptr;

    Vector<i64> children;
    collect_unignored_children(self->manager, self->node_id, children);

    gint current_link_index = 0;
    for (auto child_id : children) {
        auto const* child_data = self->manager->node(child_id);
        if (!child_data || child_data->role.bytes_as_string_view() == "text leaf"sv)
            continue;
        if (current_link_index == link_index)
            return get_or_create_hyperlink(self->manager, self->node_id, child_id);
        current_link_index++;
    }
    return nullptr;
}

static gint ladybird_hypertext_get_link_index(AtkHypertext* hypertext, gint char_offset)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(hypertext);
    if (char_offset < 0)
        return -1;

    Vector<i64> children;
    collect_unignored_children(self->manager, self->node_id, children);

    gint current_offset = 0;
    gint current_link_index = 0;
    for (auto child_id : children) {
        auto const* child_data = self->manager->node(child_id);
        if (!child_data)
            continue;
        if (child_data->role.bytes_as_string_view() == "text leaf"sv) {
            current_offset += static_cast<gint>(Utf8View(child_data->name.bytes_as_string_view()).length());
        } else {
            // The hyperlink occupies one U+FFFC character in the parent's hypertext stream.
            if (char_offset == current_offset)
                return current_link_index;
            current_offset += 1;
            current_link_index++;
        }
    }
    return -1;
}

static void ladybird_hypertext_iface_init(AtkHypertextIface* iface)
{
    iface->get_n_links = ladybird_hypertext_get_n_links;
    iface->get_link = ladybird_hypertext_get_link;
    iface->get_link_index = ladybird_hypertext_get_link_index;
}

// --- AtkImage interface ---

static char const* ladybird_image_get_description(AtkImage* image)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(image);
    auto const* data = get_node_data(self);
    if (!data)
        return "";
    // For <img>, the accessible description falls back to the alt text if no explicit description is set.
    auto text = data->description.is_empty() ? data->name.to_byte_string() : data->description.to_byte_string();
    g_free(self->cached_image_description);
    self->cached_image_description = g_strdup(text.characters());
    return self->cached_image_description;
}

static void ladybird_image_get_position(AtkImage* image, gint* x, gint* y, AtkCoordType)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(image));
    if (!data) {
        *x = *y = 0;
        return;
    }
    *x = data->bounds.x();
    *y = data->bounds.y();
}

static void ladybird_image_get_size(AtkImage* image, gint* width, gint* height)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(image));
    if (!data) {
        *width = *height = 0;
        return;
    }
    *width = data->bounds.width();
    *height = data->bounds.height();
}

static char const* ladybird_image_get_locale(AtkImage* image)
{
    // Inherit from the document's language (Orca reads this via AtkDocument for non-image objects).
    auto* self = reinterpret_cast<LadybirdAtkObject*>(image);
    auto const* data = get_node_data(self);
    if (!data || data->language.is_empty())
        return "";
    g_free(self->cached_image_locale);
    self->cached_image_locale = g_strdup(data->language.to_byte_string().characters());
    return self->cached_image_locale;
}

static void ladybird_image_iface_init(AtkImageIface* iface)
{
    iface->get_image_description = ladybird_image_get_description;
    iface->get_image_position = ladybird_image_get_position;
    iface->get_image_size = ladybird_image_get_size;
    iface->get_image_locale = ladybird_image_get_locale;
}

// --- AtkEditableText interface ---
//
// Text editing for form fields (<input>, <textarea>) via IPC. Full set_text is implemented as "replace the entire
// value" — we don't track per-character dirty state, so any edit goes through as a whole-value update.

static void ladybird_editable_text_insert_text(AtkEditableText* editable, gchar const* text, gint length,
    gint* position)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(editable);
    if (!self->text_action_callback || !text)
        return;
    auto payload = String::from_utf8(StringView { text, static_cast<size_t>(length) })
                       .release_value_but_fixme_should_propagate_errors();
    gint pos = *position;
    (*self->text_action_callback)(self->node_id, "insert_text"_string, pos, pos, payload);
    *position = pos + length;
}

static void ladybird_editable_text_delete_text(AtkEditableText* editable, gint start_pos, gint end_pos)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(editable);
    if (!self->text_action_callback)
        return;
    (*self->text_action_callback)(self->node_id, "delete_text"_string, start_pos, end_pos, String {});
}

static void ladybird_editable_text_set_text_contents(AtkEditableText* editable, gchar const* text)
{
    // Implement set_text_contents as a replace-all: delete everything, then insert.
    auto* self = reinterpret_cast<LadybirdAtkObject*>(editable);
    if (!self->text_action_callback || !text)
        return;
    auto character_count = ladybird_text_get_character_count(ATK_TEXT(editable));
    (*self->text_action_callback)(self->node_id, "delete_text"_string, 0, character_count, String {});
    auto payload = String::from_utf8({ text, strlen(text) }).release_value_but_fixme_should_propagate_errors();
    (*self->text_action_callback)(self->node_id, "insert_text"_string, 0, 0, payload);
}

static void ladybird_editable_text_iface_init(AtkEditableTextIface* iface)
{
    iface->insert_text = ladybird_editable_text_insert_text;
    iface->delete_text = ladybird_editable_text_delete_text;
    iface->set_text_contents = ladybird_editable_text_set_text_contents;
    // copy/cut/paste require clipboard integration which AT-SPI2 itself doesn't do; leaving these to Orca's
    // higher-level clipboard support which uses keyboard synthesis.
}

// --- AtkValue interface ---

static void ladybird_value_get_current_value(AtkValue* value, GValue* out)
{
    g_value_init(out, G_TYPE_DOUBLE);
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(value));
    if (data && !isnan(data->value_numeric))
        g_value_set_double(out, data->value_numeric);
}

static void ladybird_value_get_minimum_value(AtkValue* value, GValue* out)
{
    g_value_init(out, G_TYPE_DOUBLE);
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(value));
    if (data && !isnan(data->value_minimum))
        g_value_set_double(out, data->value_minimum);
}

static void ladybird_value_get_maximum_value(AtkValue* value, GValue* out)
{
    g_value_init(out, G_TYPE_DOUBLE);
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(value));
    if (data && !isnan(data->value_maximum))
        g_value_set_double(out, data->value_maximum);
}

static void ladybird_value_get_minimum_increment(AtkValue* value, GValue* out)
{
    g_value_init(out, G_TYPE_DOUBLE);
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(value));
    if (data && !isnan(data->value_step))
        g_value_set_double(out, data->value_step);
}

static void ladybird_value_iface_init(AtkValueIface* iface)
{
    iface->get_current_value = ladybird_value_get_current_value;
    iface->get_minimum_value = ladybird_value_get_minimum_value;
    iface->get_maximum_value = ladybird_value_get_maximum_value;
    iface->get_minimum_increment = ladybird_value_get_minimum_increment;
}

// --- AtkSelection interface ---
//
// AtkSelection is implemented on *container* nodes (listboxes, menus, etc.). A container's selection is the set of
// its descendants whose node_data.is_selected is true. For our purposes we only check direct children, which covers
// the common <select>/<option> and ARIA listbox patterns.

static gint ladybird_selection_get_selection_count(AtkSelection* selection)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data)
        return 0;
    gint count = 0;
    for (auto child_id : data->child_ids) {
        if (auto const* child = self->manager->node(child_id); child && child->is_selected)
            ++count;
    }
    return count;
}

static AtkObject* ladybird_selection_ref_selection(AtkSelection* selection, gint i)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || i < 0)
        return nullptr;
    gint count = 0;
    for (auto child_id : data->child_ids) {
        auto const* child = self->manager->node(child_id);
        if (!child || !child->is_selected)
            continue;
        if (count == i) {
            auto* obj = ladybird_atk_object_new(child_id, self->manager, self->action_callback);
            if (obj)
                g_object_ref(obj);
            return obj;
        }
        ++count;
    }
    return nullptr;
}

static gboolean ladybird_selection_is_child_selected(AtkSelection* selection, gint i)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || i < 0 || static_cast<size_t>(i) >= data->child_ids.size())
        return FALSE;
    auto const* child = self->manager->node(data->child_ids[i]);
    return (child && child->is_selected) ? TRUE : FALSE;
}

static gboolean ladybird_selection_add_selection(AtkSelection* selection, gint i)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || i < 0 || static_cast<size_t>(i) >= data->child_ids.size() || !self->text_action_callback)
        return FALSE;
    (*self->text_action_callback)(data->child_ids[i], "set_selected"_string, 1, 0, String {});
    return TRUE;
}

static gboolean ladybird_selection_remove_selection(AtkSelection* selection, gint i)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || i < 0 || !self->text_action_callback)
        return FALSE;
    // remove_selection takes the index within the set of currently-selected children.
    gint current = 0;
    for (auto child_id : data->child_ids) {
        auto const* child = self->manager->node(child_id);
        if (!child || !child->is_selected)
            continue;
        if (current == i) {
            (*self->text_action_callback)(child_id, "set_selected"_string, 0, 0, String {});
            return TRUE;
        }
        ++current;
    }
    return FALSE;
}

static gboolean ladybird_selection_clear_selection(AtkSelection* selection)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || !self->text_action_callback)
        return FALSE;
    for (auto child_id : data->child_ids) {
        auto const* child = self->manager->node(child_id);
        if (child && child->is_selected)
            (*self->text_action_callback)(child_id, "set_selected"_string, 0, 0, String {});
    }
    return TRUE;
}

static gboolean ladybird_selection_select_all_selection(AtkSelection* selection)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(selection);
    auto const* data = get_node_data(self);
    if (!data || !self->text_action_callback || !data->is_multi_selectable)
        return FALSE;
    for (auto child_id : data->child_ids)
        (*self->text_action_callback)(child_id, "set_selected"_string, 1, 0, String {});
    return TRUE;
}

static void ladybird_selection_iface_init(AtkSelectionIface* iface)
{
    iface->get_selection_count = ladybird_selection_get_selection_count;
    iface->ref_selection = ladybird_selection_ref_selection;
    iface->is_child_selected = ladybird_selection_is_child_selected;
    iface->add_selection = ladybird_selection_add_selection;
    iface->remove_selection = ladybird_selection_remove_selection;
    iface->clear_selection = ladybird_selection_clear_selection;
    iface->select_all_selection = ladybird_selection_select_all_selection;
}

// --- AtkTable interface ---

static gint ladybird_table_get_n_rows(AtkTable* table)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(table));
    return (data && data->table_row_count >= 0) ? data->table_row_count : 0;
}

static gint ladybird_table_get_n_columns(AtkTable* table)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(table));
    return (data && data->table_column_count >= 0) ? data->table_column_count : 0;
}

// Walk the table's descendants looking for a cell at the given row/column position.
static WebView::AccessibilityNodeData const* find_cell_at(WebView::AccessibilityTreeManager const* manager,
    WebView::AccessibilityNodeData const* table_data, gint row, gint column)
{
    if (!manager || !table_data)
        return nullptr;
    Vector<i64> stack;
    for (auto child_id : table_data->child_ids)
        stack.append(child_id);
    while (!stack.is_empty()) {
        auto id = stack.take_last();
        auto const* node = manager->node(id);
        if (!node)
            continue;
        if (node->cell_row_index == row && node->cell_column_index == column)
            return node;
        for (auto child_id : node->child_ids)
            stack.append(child_id);
    }
    return nullptr;
}

static gint ladybird_table_get_index_at(AtkTable* table, gint row, gint column)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    if (auto const* cell = find_cell_at(self->manager, data, row, column))
        return static_cast<gint>(cell->id);
    return -1;
}

static gint ladybird_table_get_column_at_index(AtkTable* table, gint index)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* node = self->manager->node(index);
    return (node && node->cell_column_index >= 0) ? node->cell_column_index : -1;
}

static gint ladybird_table_get_row_at_index(AtkTable* table, gint index)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* node = self->manager->node(index);
    return (node && node->cell_row_index >= 0) ? node->cell_row_index : -1;
}

static AtkObject* ladybird_table_ref_at(AtkTable* table, gint row, gint column)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    auto const* cell = find_cell_at(self->manager, data, row, column);
    if (!cell)
        return nullptr;
    auto* obj = ladybird_atk_object_new(cell->id, self->manager, self->action_callback);
    if (obj)
        g_object_ref(obj);
    return obj;
}

static gint ladybird_table_get_column_extent_at(AtkTable* table, gint row, gint column)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    auto const* cell = find_cell_at(self->manager, data, row, column);
    return cell ? cell->column_span : 0;
}

static gint ladybird_table_get_row_extent_at(AtkTable* table, gint row, gint column)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    auto const* cell = find_cell_at(self->manager, data, row, column);
    return cell ? cell->row_span : 0;
}

// ATK's get_column_header / get_row_header / get_caption all have (transfer none) semantics: the
// caller does not own the returned reference and must not unref it. ladybird_atk_object_new already
// returns a borrowed pointer into s_atk_object_cache, which is exactly what ATK expects here, so no
// extra g_object_ref is added.
static AtkObject* ladybird_table_get_column_header(AtkTable* table, gint column)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    if (!data || column < 0 || static_cast<size_t>(column) >= data->column_header_ids.size())
        return nullptr;
    auto header_id = data->column_header_ids[column];
    if (header_id < 0)
        return nullptr;
    return ladybird_atk_object_new(header_id, self->manager, self->action_callback, self->text_action_callback);
}

static AtkObject* ladybird_table_get_row_header(AtkTable* table, gint row)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    if (!data || row < 0 || static_cast<size_t>(row) >= data->row_header_ids.size())
        return nullptr;
    auto header_id = data->row_header_ids[row];
    if (header_id < 0)
        return nullptr;
    return ladybird_atk_object_new(header_id, self->manager, self->action_callback, self->text_action_callback);
}

static AtkObject* ladybird_table_get_caption(AtkTable* table)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    if (!data || data->table_caption_id < 0)
        return nullptr;
    return ladybird_atk_object_new(data->table_caption_id, self->manager, self->action_callback,
        self->text_action_callback);
}

static gchar const* ladybird_table_get_summary_text(AtkTable* table)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(table);
    auto const* data = get_node_data(self);
    if (!data || data->table_summary.is_empty())
        return "";
    g_free(self->cached_summary);
    self->cached_summary = g_strdup(data->table_summary.to_byte_string().characters());
    return self->cached_summary;
}

static void ladybird_table_iface_init(AtkTableIface* iface)
{
    iface->get_n_rows = ladybird_table_get_n_rows;
    iface->get_n_columns = ladybird_table_get_n_columns;
    iface->get_index_at = ladybird_table_get_index_at;
    iface->get_column_at_index = ladybird_table_get_column_at_index;
    iface->get_row_at_index = ladybird_table_get_row_at_index;
    iface->ref_at = ladybird_table_ref_at;
    iface->get_column_extent_at = ladybird_table_get_column_extent_at;
    iface->get_row_extent_at = ladybird_table_get_row_extent_at;
    iface->get_column_header = ladybird_table_get_column_header;
    iface->get_row_header = ladybird_table_get_row_header;
    iface->get_caption = ladybird_table_get_caption;
    // AtkTable's get_summary returns AtkObject, not text. Our summary is text (from the deprecated
    // HTML summary="" attribute), so leave get_summary unimplemented. AT-SPI2 clients can read the
    // text via AtkObject::get_description if needed.
    (void)ladybird_table_get_summary_text;
}

// --- AtkTableCell interface ---

static gint ladybird_table_cell_get_column_span(AtkTableCell* cell)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(cell));
    return data ? data->column_span : 1;
}

static gint ladybird_table_cell_get_row_span(AtkTableCell* cell)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(cell));
    return data ? data->row_span : 1;
}

static gboolean ladybird_table_cell_get_position(AtkTableCell* cell, gint* row, gint* column)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(cell));
    if (!data || data->cell_row_index < 0 || data->cell_column_index < 0)
        return FALSE;
    *row = data->cell_row_index;
    *column = data->cell_column_index;
    return TRUE;
}

static gboolean ladybird_table_cell_get_row_column_span(AtkTableCell* cell, gint* row, gint* column,
    gint* row_span, gint* column_span)
{
    auto const* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(cell));
    if (!data || data->cell_row_index < 0 || data->cell_column_index < 0)
        return FALSE;
    *row = data->cell_row_index;
    *column = data->cell_column_index;
    *row_span = data->row_span;
    *column_span = data->column_span;
    return TRUE;
}

// AtkTableCell::get_table has (transfer none) semantics: return a borrowed reference into the
// s_atk_object_cache, without calling g_object_ref.
static AtkObject* ladybird_table_cell_get_table(AtkTableCell* cell)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(cell);
    auto const* data = get_node_data(self);
    if (!data)
        return nullptr;
    // Walk up the parent chain to find a node with table_row_count >= 0.
    auto current_id = data->parent_id;
    while (current_id >= 0) {
        auto const* ancestor = self->manager->node(current_id);
        if (!ancestor)
            break;
        if (ancestor->table_row_count >= 0)
            return ladybird_atk_object_new(ancestor->id, self->manager, self->action_callback);
        current_id = ancestor->parent_id;
    }
    return nullptr;
}

static void ladybird_table_cell_iface_init(AtkTableCellIface* iface)
{
    iface->get_column_span = ladybird_table_cell_get_column_span;
    iface->get_row_span = ladybird_table_cell_get_row_span;
    iface->get_position = ladybird_table_cell_get_position;
    iface->get_row_column_span = ladybird_table_cell_get_row_column_span;
    iface->get_table = ladybird_table_cell_get_table;
}

// --- GType registration with all interfaces ---

GType ladybird_atk_object_get_type();
G_DEFINE_TYPE_WITH_CODE(LadybirdAtkObject, ladybird_atk_object, ATK_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(ATK_TYPE_TEXT, ladybird_text_iface_init)
        G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, ladybird_component_iface_init)
            G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, ladybird_action_iface_init)
                G_IMPLEMENT_INTERFACE(ATK_TYPE_DOCUMENT, ladybird_document_iface_init)
                    G_IMPLEMENT_INTERFACE(ATK_TYPE_HYPERTEXT, ladybird_hypertext_iface_init)
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_IMAGE, ladybird_image_iface_init)
                            G_IMPLEMENT_INTERFACE(ATK_TYPE_EDITABLE_TEXT, ladybird_editable_text_iface_init)
                                G_IMPLEMENT_INTERFACE(ATK_TYPE_VALUE, ladybird_value_iface_init)
                                    G_IMPLEMENT_INTERFACE(ATK_TYPE_SELECTION, ladybird_selection_iface_init)
                                        G_IMPLEMENT_INTERFACE(ATK_TYPE_TABLE, ladybird_table_iface_init)
                                            G_IMPLEMENT_INTERFACE(ATK_TYPE_TABLE_CELL, ladybird_table_cell_iface_init))

// --- AtkObject virtual methods ---

static char const* ladybird_atk_object_get_name(AtkObject* obj)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    auto const* data = get_node_data(self);
    if (!data)
        return "";
    g_free(self->cached_name);
    self->cached_name = g_strdup(data->name.to_byte_string().characters());
    return self->cached_name;
}

static char const* ladybird_atk_object_get_description(AtkObject* obj)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    auto const* data = get_node_data(self);
    if (!data)
        return "";
    g_free(self->cached_description);
    self->cached_description = g_strdup(data->description.to_byte_string().characters());
    return self->cached_description;
}

static AtkRole ladybird_atk_object_get_role(AtkObject* obj)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data)
        return ATK_ROLE_UNKNOWN;
    return ladybird_aria_role_to_atk_role(data->role.bytes_as_string_view());
}

static gint ladybird_atk_object_get_n_children(AtkObject* obj)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data)
        return 0;
    return static_cast<gint>(data->child_ids.size());
}

static AtkObject* ladybird_atk_object_ref_child(AtkObject* obj, gint index)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data || index < 0 || static_cast<size_t>(index) >= data->child_ids.size())
        return nullptr;
    auto child_id = data->child_ids[index];
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    auto* child = ladybird_atk_object_new(child_id, self->manager);
    if (child)
        g_object_ref(child);
    return child;
}

static AtkObject* ladybird_atk_object_get_parent(AtkObject* obj)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data || data->parent_id < 0)
        return nullptr;
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    return ladybird_atk_object_new(data->parent_id, self->manager);
}

static gint ladybird_atk_object_get_index_in_parent(AtkObject* obj)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data || data->parent_id < 0)
        return -1;
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    auto const* parent = self->manager->node(data->parent_id);
    if (!parent)
        return -1;
    for (size_t i = 0; i < parent->child_ids.size(); ++i) {
        if (parent->child_ids[i] == data->id)
            return static_cast<gint>(i);
    }
    return -1;
}

static AtkStateSet* ladybird_atk_object_ref_state_set(AtkObject* obj)
{
    auto* state_set = atk_state_set_new();
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data)
        return state_set;

    atk_state_set_add_state(state_set, ATK_STATE_FOCUSABLE);
    atk_state_set_add_state(state_set, ATK_STATE_VISIBLE);
    atk_state_set_add_state(state_set, ATK_STATE_SHOWING);
    atk_state_set_add_state(state_set, ATK_STATE_ENABLED);

    if (data->is_focused)
        atk_state_set_add_state(state_set, ATK_STATE_FOCUSED);
    if (data->is_disabled) {
        atk_state_set_remove_state(state_set, ATK_STATE_ENABLED);
        atk_state_set_remove_state(state_set, ATK_STATE_SENSITIVE);
    }

    auto role = data->role.bytes_as_string_view();
    if (role == "link"sv)
        atk_state_set_add_state(state_set, ATK_STATE_FOCUSABLE);
    if (role == "text leaf"sv)
        atk_state_set_add_state(state_set, ATK_STATE_READ_ONLY);

    // Selection / checked / expanded / editable / etc. states populated by AccessibilityTreeNode.cpp.
    if (data->is_selected) {
        atk_state_set_add_state(state_set, ATK_STATE_SELECTED);
        atk_state_set_add_state(state_set, ATK_STATE_SELECTABLE);
    } else if (role == "option"sv || role == "menuitem"sv || role == "tab"sv) {
        atk_state_set_add_state(state_set, ATK_STATE_SELECTABLE);
    }

    using CheckedState = WebView::AccessibilityNodeData::CheckedState;
    switch (data->checked_state) {
    case CheckedState::Checked:
        atk_state_set_add_state(state_set, ATK_STATE_CHECKABLE);
        atk_state_set_add_state(state_set, ATK_STATE_CHECKED);
        break;
    case CheckedState::Unchecked:
        atk_state_set_add_state(state_set, ATK_STATE_CHECKABLE);
        break;
    case CheckedState::Mixed:
        atk_state_set_add_state(state_set, ATK_STATE_CHECKABLE);
        atk_state_set_add_state(state_set, ATK_STATE_INDETERMINATE);
        break;
    case CheckedState::NotApplicable:
        break;
    }

    using ExpandedState = WebView::AccessibilityNodeData::ExpandedState;
    switch (data->expanded_state) {
    case ExpandedState::Expanded:
        atk_state_set_add_state(state_set, ATK_STATE_EXPANDABLE);
        atk_state_set_add_state(state_set, ATK_STATE_EXPANDED);
        break;
    case ExpandedState::Collapsed:
        atk_state_set_add_state(state_set, ATK_STATE_EXPANDABLE);
        break;
    case ExpandedState::NotApplicable:
        break;
    }

    if (data->is_editable)
        atk_state_set_add_state(state_set, ATK_STATE_EDITABLE);
    if (data->is_multi_line)
        atk_state_set_add_state(state_set, ATK_STATE_MULTI_LINE);
    else if (data->is_editable)
        atk_state_set_add_state(state_set, ATK_STATE_SINGLE_LINE);
    if (data->is_read_only)
        atk_state_set_add_state(state_set, ATK_STATE_READ_ONLY);
    if (data->is_required)
        atk_state_set_add_state(state_set, ATK_STATE_REQUIRED);
    if (data->is_invalid)
        atk_state_set_add_state(state_set, ATK_STATE_INVALID_ENTRY);
    if (data->is_multi_selectable)
        atk_state_set_add_state(state_set, ATK_STATE_MULTISELECTABLE);
    if (data->is_pressed)
        atk_state_set_add_state(state_set, ATK_STATE_PRESSED);
    if (data->is_visited)
        atk_state_set_add_state(state_set, ATK_STATE_VISITED);

    return state_set;
}

static AtkAttributeSet* ladybird_atk_object_get_attributes(AtkObject* obj)
{
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data)
        return nullptr;

    AtkAttributeSet* attrs = nullptr;
    auto role = data->role.bytes_as_string_view();

    // "tag" attribute — Orca's is_web_element() checks for this.
    char const* tag = nullptr;
    if (role == "document"sv)
        tag = "body";
    else if (role == "heading"sv) {
        // Scratch buffer whose contents are immediately g_strdup'd below; no long-lived lifetime needed.
        char heading_buf[16];
        snprintf(heading_buf, sizeof(heading_buf), "h%d", data->heading_level > 0 ? data->heading_level : 1);
        tag = heading_buf;
    } else if (role == "link"sv)
        tag = "a";
    else if (role == "button"sv)
        tag = "button";
    else if (role == "list"sv)
        tag = "ul";
    else if (role == "listitem"sv)
        tag = "li";
    else if (role == "table"sv)
        tag = "table";
    else if (role == "generic"sv)
        tag = "div";

    if (tag) {
        auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
        attr->name = g_strdup("tag");
        attr->value = g_strdup(tag);
        attrs = g_slist_prepend(attrs, attr);
    }

    // "xml-roles" — Orca uses this to distinguish landmarks.
    if (!role.is_empty() && role != "text leaf"sv) {
        auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
        attr->name = g_strdup("xml-roles");
        attr->value = g_strdup(ByteString(role).characters());
        attrs = g_slist_prepend(attrs, attr);
    }

    // "level" — heading level.
    if (data->heading_level > 0) {
        auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
        attr->name = g_strdup("level");
        attr->value = g_strdup(ByteString::number(data->heading_level).characters());
        attrs = g_slist_prepend(attrs, attr);
    }

    // "node-id" — internal AccessibilityNodeData ID for mapping between private bus and Qt bridge objects.
    {
        auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
        auto* attr = static_cast<AtkAttribute*>(g_malloc(sizeof(AtkAttribute)));
        attr->name = g_strdup("node-id");
        attr->value = g_strdup(ByteString::number(self->node_id).characters());
        attrs = g_slist_prepend(attrs, attr);
    }

    return attrs;
}

static AtkRelationSet* ladybird_atk_object_ref_relation_set(AtkObject* obj)
{
    auto* relation_set = ATK_OBJECT_CLASS(ladybird_atk_object_parent_class)->ref_relation_set(obj);
    auto* data = get_node_data(reinterpret_cast<LadybirdAtkObject*>(obj));
    if (!data)
        return relation_set;

    // Expose EMBEDS relation from window/frame-like roles to WebDocument children (lets Orca's active_document() find
    // the web content via the standard AT-SPI2 path).
    auto role = data->role.bytes_as_string_view();
    if (role == "document"sv || role == "generic"sv) {
        auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
        // Walk children to find a WebDocument descendant.
        for (auto child_id : data->child_ids) {
            auto const* child_data = self->manager->node(child_id);
            if (child_data && child_data->role.bytes_as_string_view() == "document"sv) {
                auto* child_obj = ladybird_atk_object_new(child_id, self->manager);
                auto* relation = atk_relation_new(&child_obj, 1, ATK_RELATION_EMBEDS);
                atk_relation_set_add(relation_set, relation);
                g_object_unref(relation);
                break;
            }
        }
    }

    return relation_set;
}

// --- GObject lifecycle ---

static void ladybird_atk_object_init(LadybirdAtkObject* self)
{
    self->node_id = -1;
    self->manager = nullptr;
    self->action_callback = nullptr;
    self->text_action_callback = nullptr;
    self->cached_name = nullptr;
    self->cached_description = nullptr;
    self->cached_keybinding = nullptr;
    self->cached_locale = nullptr;
    self->cached_image_description = nullptr;
    self->cached_image_locale = nullptr;
    self->cached_summary = nullptr;
    self->cached_hypertext = nullptr;
    self->cached_hypertext_length = 0;
    self->cached_hypertext_generation = 0;
}

static void ladybird_atk_object_finalize(GObject* obj)
{
    auto* self = reinterpret_cast<LadybirdAtkObject*>(obj);
    g_free(self->cached_name);
    g_free(self->cached_description);
    g_free(self->cached_keybinding);
    g_free(self->cached_locale);
    g_free(self->cached_image_description);
    g_free(self->cached_image_locale);
    g_free(self->cached_summary);
    g_free(self->cached_hypertext);
    s_atk_object_cache.remove(self->node_id);
    if (auto it = s_atk_hyperlink_cache.find(self->node_id); it != s_atk_hyperlink_cache.end()) {
        g_object_unref(it->value);
        s_atk_hyperlink_cache.remove(self->node_id);
    }
    G_OBJECT_CLASS(ladybird_atk_object_parent_class)->finalize(obj);
}

static void ladybird_atk_object_class_init(LadybirdAtkObjectClass* klass)
{
    auto* gobject_class = G_OBJECT_CLASS(klass);
    gobject_class->finalize = ladybird_atk_object_finalize;

    auto* atk_class = ATK_OBJECT_CLASS(klass);
    atk_class->get_name = ladybird_atk_object_get_name;
    atk_class->get_description = ladybird_atk_object_get_description;
    atk_class->get_role = ladybird_atk_object_get_role;
    atk_class->get_n_children = ladybird_atk_object_get_n_children;
    atk_class->ref_child = ladybird_atk_object_ref_child;
    atk_class->get_parent = ladybird_atk_object_get_parent;
    atk_class->get_index_in_parent = ladybird_atk_object_get_index_in_parent;
    atk_class->ref_state_set = ladybird_atk_object_ref_state_set;
    atk_class->get_attributes = ladybird_atk_object_get_attributes;
    atk_class->ref_relation_set = ladybird_atk_object_ref_relation_set;
}

// --- Role mapping ---

AtkRole ladybird_aria_role_to_atk_role(StringView role)
{
    if (role == "document"sv)
        return ATK_ROLE_DOCUMENT_WEB;
    if (role == "button"sv)
        return ATK_ROLE_PUSH_BUTTON;
    if (role == "link"sv)
        return ATK_ROLE_LINK;
    if (role == "heading"sv)
        return ATK_ROLE_HEADING;
    if (role == "textbox"sv || role == "searchbox"sv)
        return ATK_ROLE_ENTRY;
    if (role == "checkbox"sv)
        return ATK_ROLE_CHECK_BOX;
    if (role == "radio"sv)
        return ATK_ROLE_RADIO_BUTTON;
    if (role == "combobox"sv)
        return ATK_ROLE_COMBO_BOX;
    if (role == "list"sv || role == "listbox"sv)
        return ATK_ROLE_LIST;
    if (role == "listitem"sv)
        return ATK_ROLE_LIST_ITEM;
    if (role == "option"sv)
        return ATK_ROLE_LIST_ITEM;
    if (role == "group"sv)
        return ATK_ROLE_PANEL;
    if (role == "table"sv)
        return ATK_ROLE_TABLE;
    if (role == "row"sv)
        return ATK_ROLE_TABLE_ROW;
    if (role == "cell"sv || role == "gridcell"sv)
        return ATK_ROLE_TABLE_CELL;
    if (role == "columnheader"sv)
        return ATK_ROLE_TABLE_COLUMN_HEADER;
    if (role == "rowheader"sv)
        return ATK_ROLE_TABLE_ROW_HEADER;
    if (role == "img"sv || role == "image"sv)
        return ATK_ROLE_IMAGE;
    if (role == "navigation"sv || role == "main"sv || role == "banner"sv || role == "contentinfo"sv
        || role == "complementary"sv || role == "search"sv || role == "region"sv)
        return ATK_ROLE_LANDMARK;
    if (role == "form"sv)
        return ATK_ROLE_FORM;
    if (role == "dialog"sv || role == "alertdialog"sv)
        return ATK_ROLE_DIALOG;
    if (role == "progressbar"sv)
        return ATK_ROLE_PROGRESS_BAR;
    if (role == "slider"sv)
        return ATK_ROLE_SLIDER;
    if (role == "tab"sv)
        return ATK_ROLE_PAGE_TAB;
    if (role == "tablist"sv)
        return ATK_ROLE_PAGE_TAB_LIST;
    if (role == "menu"sv)
        return ATK_ROLE_MENU;
    if (role == "menuitem"sv)
        return ATK_ROLE_MENU_ITEM;
    if (role == "separator"sv)
        return ATK_ROLE_SEPARATOR;
    if (role == "alert"sv)
        return ATK_ROLE_NOTIFICATION;
    if (role == "status"sv)
        return ATK_ROLE_STATUSBAR;
    if (role == "text leaf"sv)
        return ATK_ROLE_STATIC;
    if (role == "paragraph"sv)
        return ATK_ROLE_PARAGRAPH;
    if (role == "article"sv)
        return ATK_ROLE_ARTICLE;
    if (role == "blockquote"sv)
        return ATK_ROLE_BLOCK_QUOTE;
    return ATK_ROLE_SECTION;
}

// --- Public API ---

AtkObject* ladybird_atk_object_new(i64 node_id, WebView::AccessibilityTreeManager const* manager,
    AccessibilityActionCallback* action_callback, AccessibilityTextActionCallback* text_action_callback)
{
    if (auto it = s_atk_object_cache.find(node_id); it != s_atk_object_cache.end()) {
        auto* cached = reinterpret_cast<LadybirdAtkObject*>(it->value);
        if (cached->manager == manager)
            return it->value;
        // Manager mismatch — callers swapped the tree without going through the usual teardown.
        // Drop the stale entry and rebuild against the new manager.
        g_object_unref(it->value);
        s_atk_object_cache.remove(it);
    }

    auto* obj = static_cast<LadybirdAtkObject*>(g_object_new(ladybird_atk_object_get_type(), nullptr));
    obj->node_id = node_id;
    obj->manager = manager;
    if (action_callback)
        obj->action_callback = action_callback;
    if (text_action_callback)
        obj->text_action_callback = text_action_callback;
    s_atk_object_cache.set(node_id, ATK_OBJECT(obj));
    return ATK_OBJECT(obj);
}

void ladybird_atk_object_clear_cache()
{
    // Take ownership of the pointers out of the map first, then unref. Same reasoning as in
    // ladybird_atk_object_prune_cache: unref triggers finalize, and finalize mutates
    // s_atk_object_cache, which we cannot do while iterating it here.
    Vector<AtkObject*> pending;
    pending.ensure_capacity(s_atk_object_cache.size());
    for (auto& entry : s_atk_object_cache)
        pending.append(entry.value);
    s_atk_object_cache.clear();
    for (auto* obj : pending)
        g_object_unref(obj);
}

void ladybird_atk_object_prune_cache(WebView::AccessibilityTreeManager const& manager)
{
    Vector<i64> ids_to_remove;
    for (auto& entry : s_atk_object_cache) {
        if (!manager.node(entry.key))
            ids_to_remove.append(entry.key);
    }
    for (auto id : ids_to_remove) {
        auto it = s_atk_object_cache.find(id);
        if (it == s_atk_object_cache.end())
            continue;
        // Remove from the map FIRST, then unref. If we unref first and that was the last reference,
        // ladybird_atk_object_finalize() runs synchronously and also calls
        // s_atk_object_cache.remove(self->node_id); our subsequent remove-by-iterator would then
        // double-delete the bucket. By clearing our entry up front we make finalize's remove() a
        // no-op (key already absent), and the iterator we held is the only one that mattered.
        auto* obj = it->value;
        s_atk_object_cache.remove(it);
        g_object_unref(obj);
    }
}

}
