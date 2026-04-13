/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AccessibilityNodeData.h>
#include <LibWebView/AccessibilityTreeManager.h>
#include <UI/Accessibility/Linux/LadybirdAtkObject.h>

#include <atk/atk.h>
#include <cstring>

// Some of the ATK accessors we exercise below were marked deprecated in newer ATK headers
// in favor of newer entry points (atk_value_get_value_and_text, atk_object_get_object_locale).
// Our bridge still implements the older iface vtable slots that Orca uses, so the tests
// deliberately call through the older wrappers.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

// Helpers -----------------------------------------------------------------------------------------

namespace {

// Build a manager from a list of nodes and return it alongside a LadybirdAtkObject for the first node.
struct Fixture {
    WebView::AccessibilityTreeManager manager;
    AtkObject* object { nullptr };
};

Fixture make_fixture(Vector<WebView::AccessibilityNodeData> nodes)
{
    // Static cache inside LadybirdAtkObject would otherwise hand out objects from prior fixtures
    // whose manager has since been destroyed.
    Ladybird::ladybird_atk_object_clear_cache();

    Fixture fixture;
    auto root_id = nodes.is_empty() ? -1 : nodes[0].id;
    fixture.manager.update_tree(move(nodes));
    if (root_id >= 0)
        fixture.object = Ladybird::ladybird_atk_object_new(root_id, &fixture.manager);
    return fixture;
}

WebView::AccessibilityNodeData make_node(i64 id, String role, String name = {})
{
    WebView::AccessibilityNodeData node;
    node.id = id;
    node.role = move(role);
    node.name = move(name);
    return node;
}

StringView view(char const* s)
{
    return s ? StringView { s, strlen(s) } : StringView {};
}

}

// AtkObject basics --------------------------------------------------------------------------------

TEST_CASE(ladybird_atk_object_reports_name_and_role)
{
    auto fixture = make_fixture({ make_node(1, "button"_string, "Click me"_string) });
    EXPECT_NE(fixture.object, nullptr);

    EXPECT_EQ(view(atk_object_get_name(fixture.object)), "Click me"sv);
    EXPECT_EQ(atk_object_get_role(fixture.object), ATK_ROLE_PUSH_BUTTON);
}

TEST_CASE(ladybird_atk_object_reports_description)
{
    auto node = make_node(1, "button"_string, "Go"_string);
    node.description = "Submit the form"_string;
    auto fixture = make_fixture({ node });

    EXPECT_EQ(view(atk_object_get_description(fixture.object)), "Submit the form"sv);
}

TEST_CASE(ladybird_atk_object_maps_aria_role_to_atk_role)
{
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("link"sv), ATK_ROLE_LINK);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("heading"sv), ATK_ROLE_HEADING);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("checkbox"sv), ATK_ROLE_CHECK_BOX);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("option"sv), ATK_ROLE_LIST_ITEM);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("listbox"sv), ATK_ROLE_LIST);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("document"sv), ATK_ROLE_DOCUMENT_WEB);
    EXPECT_EQ(Ladybird::ladybird_aria_role_to_atk_role("nonsense"sv), ATK_ROLE_SECTION);
}

// State set ---------------------------------------------------------------------------------------

TEST_CASE(state_set_reports_checked_for_checkbox)
{
    auto node = make_node(1, "checkbox"_string, "Accept"_string);
    node.checked_state = WebView::AccessibilityNodeData::CheckedState::Checked;
    auto fixture = make_fixture({ node });

    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_CHECKED));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_CHECKABLE));
    g_object_unref(states);
}

TEST_CASE(state_set_reports_mixed_for_indeterminate_checkbox)
{
    auto node = make_node(1, "checkbox"_string, "Partial"_string);
    node.checked_state = WebView::AccessibilityNodeData::CheckedState::Mixed;
    auto fixture = make_fixture({ node });

    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_INDETERMINATE));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_CHECKABLE));
    EXPECT(!atk_state_set_contains_state(states, ATK_STATE_CHECKED));
    g_object_unref(states);
}

TEST_CASE(state_set_reports_expanded_for_details)
{
    auto node = make_node(1, "group"_string, "Section"_string);
    node.expanded_state = WebView::AccessibilityNodeData::ExpandedState::Expanded;
    auto fixture = make_fixture({ node });

    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_EXPANDED));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_EXPANDABLE));
    g_object_unref(states);
}

TEST_CASE(state_set_reports_required_readonly_invalid_for_inputs)
{
    auto node = make_node(1, "textbox"_string);
    node.is_editable = true;
    node.is_required = true;
    node.is_read_only = true;
    node.is_invalid = true;
    auto fixture = make_fixture({ node });

    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_EDITABLE));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_REQUIRED));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_READ_ONLY));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_INVALID_ENTRY));
    g_object_unref(states);
}

TEST_CASE(state_set_reports_multi_line_vs_single_line)
{
    auto single = make_node(1, "textbox"_string);
    single.is_editable = true;
    auto single_fixture = make_fixture({ single });
    auto* single_states = atk_object_ref_state_set(single_fixture.object);
    EXPECT(atk_state_set_contains_state(single_states, ATK_STATE_SINGLE_LINE));
    EXPECT(!atk_state_set_contains_state(single_states, ATK_STATE_MULTI_LINE));
    g_object_unref(single_states);

    auto multi = make_node(2, "textbox"_string);
    multi.is_editable = true;
    multi.is_multi_line = true;
    auto multi_fixture = make_fixture({ multi });
    auto* multi_states = atk_object_ref_state_set(multi_fixture.object);
    EXPECT(atk_state_set_contains_state(multi_states, ATK_STATE_MULTI_LINE));
    EXPECT(!atk_state_set_contains_state(multi_states, ATK_STATE_SINGLE_LINE));
    g_object_unref(multi_states);
}

TEST_CASE(state_set_reports_selected_and_selectable)
{
    auto selected = make_node(1, "option"_string, "A"_string);
    selected.is_selected = true;
    auto fixture = make_fixture({ selected });
    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_SELECTED));
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_SELECTABLE));
    g_object_unref(states);
}

TEST_CASE(state_set_reports_pressed_for_toggle_button)
{
    auto node = make_node(1, "button"_string, "Bold"_string);
    node.is_pressed = true;
    auto fixture = make_fixture({ node });
    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(atk_state_set_contains_state(states, ATK_STATE_PRESSED));
    g_object_unref(states);
}

TEST_CASE(state_set_removes_enabled_when_disabled)
{
    auto node = make_node(1, "button"_string, "Disabled"_string);
    node.is_disabled = true;
    auto fixture = make_fixture({ node });
    auto* states = atk_object_ref_state_set(fixture.object);
    EXPECT(!atk_state_set_contains_state(states, ATK_STATE_ENABLED));
    EXPECT(!atk_state_set_contains_state(states, ATK_STATE_SENSITIVE));
    g_object_unref(states);
}

// AtkText -----------------------------------------------------------------------------------------

TEST_CASE(text_reports_name_as_text_for_text_leaf)
{
    auto node = make_node(1, "text leaf"_string, "Hello, world!"_string);
    auto fixture = make_fixture({ node });

    auto* text = ATK_TEXT(fixture.object);
    auto* full = atk_text_get_text(text, 0, -1);
    EXPECT_EQ(view(full), "Hello, world!"sv);
    g_free(full);

    EXPECT_EQ(atk_text_get_character_count(text), 13);
    EXPECT_EQ(atk_text_get_character_at_offset(text, 7), static_cast<gunichar>('w'));
}

TEST_CASE(text_offsets_use_character_not_byte_positions_for_astral_codepoints)
{
    // "a👋b" is 3 characters but 6 UTF-8 bytes (1 + 4 + 1).
    auto node = make_node(1, "text leaf"_string, "a\xF0\x9F\x91\x8B"
                                                 "b"_string);
    auto fixture = make_fixture({ node });

    auto* text = ATK_TEXT(fixture.object);
    EXPECT_EQ(atk_text_get_character_count(text), 3);
    EXPECT_EQ(atk_text_get_character_at_offset(text, 0), static_cast<gunichar>('a'));
    EXPECT_EQ(atk_text_get_character_at_offset(text, 1), static_cast<gunichar>(0x1F44B)); // 👋
    EXPECT_EQ(atk_text_get_character_at_offset(text, 2), static_cast<gunichar>('b'));
}

TEST_CASE(text_word_boundary_navigation)
{
    auto node = make_node(1, "text leaf"_string, "Hello world foo bar"_string);
    auto fixture = make_fixture({ node });

    auto* text = ATK_TEXT(fixture.object);
    gint start = 0;
    gint end = 0;
    auto* segment = atk_text_get_string_at_offset(text, 7, ATK_TEXT_GRANULARITY_WORD, &start, &end);
    EXPECT_EQ(view(segment), "world"sv);
    EXPECT_EQ(start, 6);
    EXPECT_EQ(end, 11);
    g_free(segment);
}

TEST_CASE(text_caret_offset_follows_is_focused)
{
    auto focused = make_node(1, "textbox"_string);
    focused.caret_offset = 3;
    auto fixture = make_fixture({ focused });

    EXPECT_EQ(atk_text_get_caret_offset(ATK_TEXT(fixture.object)), 3);
}

TEST_CASE(text_selection_range_round_trips)
{
    auto node = make_node(1, "textbox"_string);
    node.name = "Hello world"_string;
    node.selection_start = 6;
    node.selection_end = 11;
    auto fixture = make_fixture({ node });

    auto* text = ATK_TEXT(fixture.object);
    EXPECT_EQ(atk_text_get_n_selections(text), 1);
    gint start = -1;
    gint end = -1;
    auto* selected = atk_text_get_selection(text, 0, &start, &end);
    EXPECT_EQ(start, 6);
    EXPECT_EQ(end, 11);
    g_free(selected);
}

// AtkHypertext / AtkHyperlink --------------------------------------------------------------------

TEST_CASE(hypertext_exposes_link_children_as_hyperlinks)
{
    // Paragraph containing: "Hello " (text leaf), link, " world" (text leaf).
    auto paragraph = make_node(1, "paragraph"_string);
    paragraph.child_ids = { 2, 3, 4 };

    auto prefix = make_node(2, "text leaf"_string, "Hello "_string);
    prefix.parent_id = 1;

    auto link = make_node(3, "link"_string, "click"_string);
    link.parent_id = 1;
    link.url = "https://example.org/"_string;
    link.child_ids = { 5 };

    auto link_text = make_node(5, "text leaf"_string, "click"_string);
    link_text.parent_id = 3;

    auto suffix = make_node(4, "text leaf"_string, " world"_string);
    suffix.parent_id = 1;

    auto fixture = make_fixture({ paragraph, prefix, link, suffix, link_text });

    auto* hypertext = ATK_HYPERTEXT(fixture.object);
    EXPECT_EQ(atk_hypertext_get_n_links(hypertext), 1);

    auto* hyperlink = atk_hypertext_get_link(hypertext, 0);
    EXPECT_NE(hyperlink, nullptr);
    EXPECT_EQ(atk_hyperlink_get_start_index(hyperlink), 6); // After "Hello "
    EXPECT_EQ(atk_hyperlink_get_end_index(hyperlink), 7);   // 6 + 1 character for U+FFFC

    auto* uri = atk_hyperlink_get_uri(hyperlink, 0);
    EXPECT_EQ(view(uri), "https://example.org/"sv);
    g_free(uri);

    // Character offset 6 is in the U+FFFC span, so it maps to link index 0.
    EXPECT_EQ(atk_hypertext_get_link_index(hypertext, 6), 0);
    // Character offset 3 is in "Hello ", no link there.
    EXPECT_EQ(atk_hypertext_get_link_index(hypertext, 3), -1);
}

// AtkValue ---------------------------------------------------------------------------------------

TEST_CASE(value_interface_reports_numeric_range)
{
    auto slider = make_node(1, "slider"_string, "Volume"_string);
    slider.value_numeric = 50.0;
    slider.value_minimum = 0.0;
    slider.value_maximum = 100.0;
    slider.value_step = 5.0;
    auto fixture = make_fixture({ slider });

    auto* value_iface = ATK_VALUE(fixture.object);
    GValue out = G_VALUE_INIT;
    atk_value_get_current_value(value_iface, &out);
    EXPECT_EQ(g_value_get_double(&out), 50.0);
    g_value_unset(&out);

    GValue min = G_VALUE_INIT;
    atk_value_get_minimum_value(value_iface, &min);
    EXPECT_EQ(g_value_get_double(&min), 0.0);
    g_value_unset(&min);

    GValue max = G_VALUE_INIT;
    atk_value_get_maximum_value(value_iface, &max);
    EXPECT_EQ(g_value_get_double(&max), 100.0);
    g_value_unset(&max);
}

// AtkSelection -----------------------------------------------------------------------------------

TEST_CASE(selection_interface_counts_selected_children)
{
    auto listbox = make_node(1, "listbox"_string);
    listbox.child_ids = { 2, 3, 4 };

    auto opt_a = make_node(2, "option"_string, "A"_string);
    opt_a.parent_id = 1;
    opt_a.is_selected = true;

    auto opt_b = make_node(3, "option"_string, "B"_string);
    opt_b.parent_id = 1;

    auto opt_c = make_node(4, "option"_string, "C"_string);
    opt_c.parent_id = 1;
    opt_c.is_selected = true;

    auto fixture = make_fixture({ listbox, opt_a, opt_b, opt_c });

    auto* selection = ATK_SELECTION(fixture.object);
    EXPECT_EQ(atk_selection_get_selection_count(selection), 2);
    EXPECT(atk_selection_is_child_selected(selection, 0));
    EXPECT(!atk_selection_is_child_selected(selection, 1));
    EXPECT(atk_selection_is_child_selected(selection, 2));
}

// AtkTable ---------------------------------------------------------------------------------------

TEST_CASE(table_interface_reports_dimensions_and_cell_lookup)
{
    // 2x2 table with a header row.
    auto table = make_node(1, "table"_string, "Sales"_string);
    table.child_ids = { 2, 3, 4, 5 };
    table.table_row_count = 2;
    table.table_column_count = 2;
    table.column_header_ids = { 2, 3 };
    table.row_header_ids = { -1, 4 };

    auto header1 = make_node(2, "columnheader"_string, "Quarter"_string);
    header1.parent_id = 1;
    header1.cell_row_index = 0;
    header1.cell_column_index = 0;

    auto header2 = make_node(3, "columnheader"_string, "Revenue"_string);
    header2.parent_id = 1;
    header2.cell_row_index = 0;
    header2.cell_column_index = 1;

    auto row_header = make_node(4, "rowheader"_string, "Q1"_string);
    row_header.parent_id = 1;
    row_header.cell_row_index = 1;
    row_header.cell_column_index = 0;

    auto cell = make_node(5, "cell"_string, "100"_string);
    cell.parent_id = 1;
    cell.cell_row_index = 1;
    cell.cell_column_index = 1;

    auto fixture = make_fixture({ table, header1, header2, row_header, cell });

    auto* table_iface = ATK_TABLE(fixture.object);
    EXPECT_EQ(atk_table_get_n_rows(table_iface), 2);
    EXPECT_EQ(atk_table_get_n_columns(table_iface), 2);

    auto* cell_at = atk_table_ref_at(table_iface, 1, 1);
    EXPECT_NE(cell_at, nullptr);
    EXPECT_EQ(view(atk_object_get_name(cell_at)), "100"sv);
    g_object_unref(cell_at);

    auto* col_header = atk_table_get_column_header(table_iface, 0);
    EXPECT_NE(col_header, nullptr);
    EXPECT_EQ(view(atk_object_get_name(col_header)), "Quarter"sv);
    g_object_unref(col_header);

    auto* row_header_obj = atk_table_get_row_header(table_iface, 1);
    EXPECT_NE(row_header_obj, nullptr);
    EXPECT_EQ(view(atk_object_get_name(row_header_obj)), "Q1"sv);
    g_object_unref(row_header_obj);
}

// AtkImage ---------------------------------------------------------------------------------------

TEST_CASE(image_interface_reports_description_and_size)
{
    auto image = make_node(1, "img"_string, "A descriptive label"_string);
    image.description = "Alt text"_string;
    image.bounds = { 10, 20, 100, 50 };
    auto fixture = make_fixture({ image });

    auto* image_iface = ATK_IMAGE(fixture.object);
    EXPECT_EQ(view(atk_image_get_image_description(image_iface)), "Alt text"sv);

    gint w = 0;
    gint h = 0;
    atk_image_get_image_size(image_iface, &w, &h);
    EXPECT_EQ(w, 100);
    EXPECT_EQ(h, 50);

    gint x = 0;
    gint y = 0;
    atk_image_get_image_position(image_iface, &x, &y, ATK_XY_SCREEN);
    EXPECT_EQ(x, 10);
    EXPECT_EQ(y, 20);
}

// AtkAction --------------------------------------------------------------------------------------

TEST_CASE(action_interface_reports_role_specific_names)
{
    auto link = make_node(1, "link"_string, "Home"_string);
    auto link_fixture = make_fixture({ link });
    auto* link_action = ATK_ACTION(link_fixture.object);
    EXPECT_EQ(view(atk_action_get_name(link_action, 0)), "jump"sv);

    auto checkbox = make_node(2, "checkbox"_string);
    auto checkbox_fixture = make_fixture({ checkbox });
    EXPECT_EQ(view(atk_action_get_name(ATK_ACTION(checkbox_fixture.object), 0)), "toggle"sv);

    auto option = make_node(3, "option"_string);
    auto option_fixture = make_fixture({ option });
    EXPECT_EQ(view(atk_action_get_name(ATK_ACTION(option_fixture.object), 0)), "select"sv);

    auto button = make_node(4, "button"_string);
    auto button_fixture = make_fixture({ button });
    EXPECT_EQ(view(atk_action_get_name(ATK_ACTION(button_fixture.object), 0)), "click"sv);
}

TEST_CASE(action_interface_reports_keybinding_from_accesskey)
{
    auto node = make_node(1, "button"_string, "Save"_string);
    node.keybinding = "s"_string;
    auto fixture = make_fixture({ node });

    EXPECT_EQ(view(atk_action_get_keybinding(ATK_ACTION(fixture.object), 0)), "s"sv);
}

// AtkDocument ------------------------------------------------------------------------------------

TEST_CASE(document_interface_reports_language)
{
    auto document = make_node(1, "document"_string);
    document.language = "fr-CA"_string;
    auto fixture = make_fixture({ document });

    auto* doc = ATK_DOCUMENT(fixture.object);
    EXPECT_EQ(view(atk_document_get_locale(doc)), "fr-CA"sv);
}

// AtkAction do_action routing -----------------------------------------------------------------

TEST_CASE(action_do_action_routes_to_callback)
{
    Ladybird::ladybird_atk_object_clear_cache();

    struct Invocation {
        i64 node_id { 0 };
        String action;
    };
    Vector<Invocation> invocations;
    Ladybird::AccessibilityActionCallback callback = [&](i64 node_id, String action) {
        invocations.append({ node_id, move(action) });
    };

    WebView::AccessibilityTreeManager manager;
    manager.update_tree({ make_node(42, "button"_string, "Submit"_string) });
    auto* object = Ladybird::ladybird_atk_object_new(42, &manager, &callback);

    auto* action_iface = ATK_ACTION(object);
    EXPECT_EQ(atk_action_get_n_actions(action_iface), 2);

    EXPECT(atk_action_do_action(action_iface, 0));
    EXPECT(atk_action_do_action(action_iface, 1));
    EXPECT(!atk_action_do_action(action_iface, 7));

    EXPECT_EQ(invocations.size(), 2u);
    EXPECT_EQ(invocations[0].node_id, 42);
    EXPECT_EQ(invocations[0].action, "press"sv);
    EXPECT_EQ(invocations[1].node_id, 42);
    EXPECT_EQ(invocations[1].action, "focus"sv);
}

TEST_CASE(action_do_action_noop_when_callback_missing)
{
    // With no callback installed, do_action must return FALSE without crashing.
    auto fixture = make_fixture({ make_node(1, "button"_string, "Go"_string) });
    EXPECT(!atk_action_do_action(ATK_ACTION(fixture.object), 0));
}

// AtkComponent geometry ----------------------------------------------------------------------

TEST_CASE(component_reports_extents_position_and_size)
{
    auto node = make_node(1, "button"_string, "Press"_string);
    node.bounds = { 30, 40, 120, 24 };
    auto fixture = make_fixture({ node });

    auto* component = ATK_COMPONENT(fixture.object);

    gint x = 0;
    gint y = 0;
    gint w = 0;
    gint h = 0;
    atk_component_get_extents(component, &x, &y, &w, &h, ATK_XY_SCREEN);
    EXPECT_EQ(x, 30);
    EXPECT_EQ(y, 40);
    EXPECT_EQ(w, 120);
    EXPECT_EQ(h, 24);

    gint px = 0;
    gint py = 0;
    atk_component_get_position(component, &px, &py, ATK_XY_SCREEN);
    EXPECT_EQ(px, 30);
    EXPECT_EQ(py, 40);

    gint sw = 0;
    gint sh = 0;
    atk_component_get_size(component, &sw, &sh);
    EXPECT_EQ(sw, 120);
    EXPECT_EQ(sh, 24);
}

// AtkTableCell --------------------------------------------------------------------------------

TEST_CASE(table_cell_reports_spans_position_and_parent_table)
{
    auto table = make_node(1, "table"_string, "Grid"_string);
    table.child_ids = { 2 };
    table.table_row_count = 3;
    table.table_column_count = 4;

    auto cell = make_node(2, "cell"_string, "Merged"_string);
    cell.parent_id = 1;
    cell.cell_row_index = 1;
    cell.cell_column_index = 2;
    cell.row_span = 2;
    cell.column_span = 3;

    Ladybird::ladybird_atk_object_clear_cache();
    WebView::AccessibilityTreeManager manager;
    manager.update_tree({ table, cell });
    auto* cell_obj = Ladybird::ladybird_atk_object_new(2, &manager);

    auto* iface = ATK_TABLE_CELL(cell_obj);
    EXPECT_EQ(atk_table_cell_get_column_span(iface), 3);
    EXPECT_EQ(atk_table_cell_get_row_span(iface), 2);

    gint row = -1;
    gint col = -1;
    EXPECT(atk_table_cell_get_position(iface, &row, &col));
    EXPECT_EQ(row, 1);
    EXPECT_EQ(col, 2);

    gint r = -1;
    gint c = -1;
    gint rs = -1;
    gint cs = -1;
    EXPECT(atk_table_cell_get_row_column_span(iface, &r, &c, &rs, &cs));
    EXPECT_EQ(r, 1);
    EXPECT_EQ(c, 2);
    EXPECT_EQ(rs, 2);
    EXPECT_EQ(cs, 3);

    auto* parent_table = atk_table_cell_get_table(iface);
    EXPECT_NE(parent_table, nullptr);
    EXPECT_EQ(view(atk_object_get_name(parent_table)), "Grid"sv);
    g_object_unref(parent_table);
}

TEST_CASE(table_cell_position_returns_false_for_unpositioned_cell)
{
    auto cell = make_node(1, "cell"_string);
    // Leave cell_row_index / cell_column_index at their defaults (-1).
    auto fixture = make_fixture({ cell });

    gint row = 99;
    gint col = 99;
    EXPECT(!atk_table_cell_get_position(ATK_TABLE_CELL(fixture.object), &row, &col));
}

// AtkObject::get_attributes -------------------------------------------------------------------

namespace {

Optional<StringView> find_attribute(AtkAttributeSet* attrs, StringView name)
{
    for (auto* iter = attrs; iter; iter = iter->next) {
        auto* attr = static_cast<AtkAttribute*>(iter->data);
        if (view(attr->name) == name)
            return view(attr->value);
    }
    return {};
}

}

TEST_CASE(attributes_include_tag_and_xml_roles_for_heading)
{
    auto node = make_node(5, "heading"_string, "Chapter 1"_string);
    node.heading_level = 2;
    auto fixture = make_fixture({ node });

    auto* attrs = atk_object_get_attributes(fixture.object);
    EXPECT_EQ(find_attribute(attrs, "tag"sv), "h2"sv);
    EXPECT_EQ(find_attribute(attrs, "xml-roles"sv), "heading"sv);
    EXPECT_EQ(find_attribute(attrs, "level"sv), "2"sv);
    EXPECT_EQ(find_attribute(attrs, "node-id"sv), "5"sv);
    atk_attribute_set_free(attrs);
}

TEST_CASE(attributes_tag_for_common_roles)
{
    struct Expectation {
        StringView role;
        StringView expected_tag;
    };
    Array<Expectation, 6> cases { {
        { "link"sv, "a"sv },
        { "button"sv, "button"sv },
        { "list"sv, "ul"sv },
        { "listitem"sv, "li"sv },
        { "table"sv, "table"sv },
        { "generic"sv, "div"sv },
    } };

    for (auto const& [role, expected_tag] : cases) {
        auto fixture = make_fixture({ make_node(1, MUST(String::from_utf8(role))) });
        auto* attrs = atk_object_get_attributes(fixture.object);
        auto tag = find_attribute(attrs, "tag"sv);
        EXPECT(tag.has_value());
        EXPECT_EQ(*tag, expected_tag);
        atk_attribute_set_free(attrs);
    }
}

TEST_CASE(attributes_text_leaf_omits_xml_roles)
{
    auto fixture = make_fixture({ make_node(1, "text leaf"_string, "hi"_string) });
    auto* attrs = atk_object_get_attributes(fixture.object);
    // Text leaves get a node-id but no xml-roles — Orca skips them anyway.
    EXPECT(!find_attribute(attrs, "xml-roles"sv).has_value());
    EXPECT(find_attribute(attrs, "node-id"sv).has_value());
    atk_attribute_set_free(attrs);
}

// Tree walking --------------------------------------------------------------------------------

TEST_CASE(tree_walking_parent_and_index_in_parent)
{
    auto root = make_node(1, "document"_string);
    root.child_ids = { 2, 3, 4 };

    auto a = make_node(2, "button"_string, "A"_string);
    a.parent_id = 1;
    auto b = make_node(3, "button"_string, "B"_string);
    b.parent_id = 1;
    auto c = make_node(4, "button"_string, "C"_string);
    c.parent_id = 1;

    Ladybird::ladybird_atk_object_clear_cache();
    WebView::AccessibilityTreeManager manager;
    manager.update_tree({ root, a, b, c });

    auto* root_obj = Ladybird::ladybird_atk_object_new(1, &manager);
    EXPECT_EQ(atk_object_get_n_accessible_children(root_obj), 3);

    auto* middle_child = atk_object_ref_accessible_child(root_obj, 1);
    EXPECT_NE(middle_child, nullptr);
    EXPECT_EQ(view(atk_object_get_name(middle_child)), "B"sv);
    EXPECT_EQ(atk_object_get_index_in_parent(middle_child), 1);

    auto* parent = atk_object_get_parent(middle_child);
    EXPECT_NE(parent, nullptr);
    EXPECT_EQ(view(atk_object_get_name(parent)), view(atk_object_get_name(root_obj)));

    g_object_unref(middle_child);
}

TEST_CASE(hypertext_collapses_empty_generic_wrappers_into_text)
{
    // <p>Hello <span><span>world</span></span></p> — the two empty generics wrap a single
    // "world" text leaf. build_hypertext should see "Hello world" with no link placeholders,
    // because is_ignored_role collapses the empty generics.
    auto paragraph = make_node(1, "paragraph"_string);
    paragraph.child_ids = { 2, 3 };

    auto hello = make_node(2, "text leaf"_string, "Hello "_string);
    hello.parent_id = 1;

    auto outer_span = make_node(3, "generic"_string);
    outer_span.parent_id = 1;
    outer_span.child_ids = { 4 };

    auto inner_span = make_node(4, "generic"_string);
    inner_span.parent_id = 3;
    inner_span.child_ids = { 5 };

    auto world = make_node(5, "text leaf"_string, "world"_string);
    world.parent_id = 4;

    auto fixture = make_fixture({ paragraph, hello, outer_span, inner_span, world });

    auto* text = ATK_TEXT(fixture.object);
    auto* full = atk_text_get_text(text, 0, -1);
    EXPECT_EQ(view(full), "Hello world"sv);
    g_free(full);

    // No hyperlinks since the only non-text descendants are ignored generics.
    EXPECT_EQ(atk_hypertext_get_n_links(ATK_HYPERTEXT(fixture.object)), 0);
}

// AtkObject::ref_relation_set (EMBEDS) --------------------------------------------------------

TEST_CASE(relation_set_exposes_embeds_from_generic_wrapper_to_document)
{
    auto wrapper = make_node(1, "generic"_string);
    wrapper.child_ids = { 2 };

    auto document = make_node(2, "document"_string, "Page"_string);
    document.parent_id = 1;

    auto fixture = make_fixture({ wrapper, document });

    auto* relation_set = atk_object_ref_relation_set(fixture.object);
    EXPECT_NE(relation_set, nullptr);

    auto* embeds = atk_relation_set_get_relation_by_type(relation_set, ATK_RELATION_EMBEDS);
    EXPECT_NE(embeds, nullptr);

    auto* targets = atk_relation_get_target(embeds);
    EXPECT_EQ(targets->len, 1u);
    auto* target = static_cast<AtkObject*>(g_ptr_array_index(targets, 0));
    EXPECT_EQ(view(atk_object_get_name(target)), "Page"sv);

    g_object_unref(relation_set);
}

// State-change signal emission ----------------------------------------------------------------

namespace {

struct StateChangeRecorder {
    String state_name;
    bool state_value { false };
    bool fired { false };
};

void on_state_change(AtkObject*, char* state_name, gboolean state_value, gpointer user_data)
{
    auto* recorder = static_cast<StateChangeRecorder*>(user_data);
    recorder->state_name = MUST(String::from_utf8(view(state_name)));
    recorder->state_value = state_value != FALSE;
    recorder->fired = true;
}

}

TEST_CASE(state_change_signal_emits_on_notify_state_change)
{
    auto fixture = make_fixture({ make_node(1, "textbox"_string) });

    StateChangeRecorder recorder;
    g_signal_connect(fixture.object, "state-change", G_CALLBACK(on_state_change), &recorder);

    atk_object_notify_state_change(fixture.object, ATK_STATE_FOCUSED, TRUE);
    EXPECT(recorder.fired);
    EXPECT_EQ(recorder.state_name, "focused"sv);
    EXPECT(recorder.state_value);
}

TEST_CASE(focus_event_signal_emits)
{
    auto fixture = make_fixture({ make_node(1, "textbox"_string) });

    struct FocusRecorder {
        bool fired { false };
        bool value { false };
    } recorder;
    auto handler = +[](AtkObject*, gboolean focus_in, gpointer user_data) {
        auto* r = static_cast<FocusRecorder*>(user_data);
        r->fired = true;
        r->value = focus_in != FALSE;
    };

    g_signal_connect(fixture.object, "focus-event", G_CALLBACK(handler), &recorder);
    g_signal_emit_by_name(fixture.object, "focus-event", TRUE);
    EXPECT(recorder.fired);
    EXPECT(recorder.value);
}

#pragma GCC diagnostic pop
