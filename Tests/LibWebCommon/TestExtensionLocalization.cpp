/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebCommon/Extensions/Localization.h>

using namespace Web::Extensions;

TEST_CASE(catalog_names_are_case_insensitive)
{
    auto messages = MUST(parse_message_catalog(R"({"HeLLo":{"message":"World","description":42},"@ok_1":{"message":"yes"}})"sv));
    EXPECT_EQ(messages.get("hello"sv).value(), "World"sv);
    EXPECT(is_valid_message_name("@ok_1"sv));
    EXPECT(!is_valid_message_name(""sv));
    EXPECT(!is_valid_message_name("not-valid"sv));
}

TEST_CASE(placeholders_resolve_at_parse)
{
    auto messages = MUST(parse_message_catalog(R"({"a":{"message":"Hi $NAME$! $bad-name$ $name$", "placeholders":{"Name":{"content":"friend"}}}})"sv));
    EXPECT_EQ(messages.get("a"sv).value(), "Hi friend! $bad-name$ friend"sv);
}

TEST_CASE(placeholder_content_keeps_positional_markers)
{
    auto messages = MUST(parse_message_catalog(R"({"a":{"message":"$who$", "placeholders":{"who":{"content":"$1 $other$"}}}})"sv));
    EXPECT_EQ(messages.get("a"sv).value(), "$1 $other$"sv);
}

static void expect_error(StringView json, StringView expected)
{
    auto result = parse_message_catalog(json);
    EXPECT(result.is_error());
    if (result.is_error())
        EXPECT_EQ(result.error(), expected);
}

TEST_CASE(catalog_invalid_json)
{
    expect_error("{"sv, "not valid JSON"sv);
    expect_error("[]"sv, "the top level must be an object"sv);
}

TEST_CASE(catalog_invalid_tree)
{
    expect_error(R"({"key":1})"sv, "\"key\" must be an object"sv);
}

TEST_CASE(catalog_missing_message)
{
    expect_error(R"({"key":{}})"sv, "\"key\" needs a \"message\" string"sv);
    expect_error(R"({"key":{"message":1}})"sv, "\"key\" needs a \"message\" string"sv);
}

TEST_CASE(catalog_invalid_placeholders)
{
    expect_error(R"({"key":{"message":"", "placeholders":[]}})"sv, "\"key\": \"placeholders\" must be an object"sv);
}

TEST_CASE(catalog_invalid_placeholder)
{
    expect_error(R"({"key":{"message":"", "placeholders":{"who":1}}})"sv, "\"key\": placeholder \"who\" must be an object"sv);
}

TEST_CASE(catalog_invalid_content)
{
    expect_error(R"({"key":{"message":"", "placeholders":{"who":{}}}})"sv, "\"key\": placeholder \"who\" needs a \"content\" string"sv);
}

TEST_CASE(catalog_invalid_name)
{
    expect_error(R"({"bad-name":{"message":""}})"sv, "\"bad-name\" isn't a valid name: only ASCII letters, digits, \"_\", and \"@\" are allowed"sv);
    expect_error(R"({"key":{"message":"", "placeholders":{"bad-name":{"content":""}}}})"sv, "\"bad-name\" isn't a valid name: only ASCII letters, digits, \"_\", and \"@\" are allowed"sv);
}

TEST_CASE(catalog_undefined_placeholder)
{
    expect_error(R"({"key":{"message":"$Who$"}})"sv, "\"key\": \"$Who$\" isn't defined"sv);
}

TEST_CASE(catalog_reserved_name)
{
    expect_error(R"({"@@UI_locale":{"message":"fake"}})"sv, "\"@@UI_locale\" is a reserved name"sv);
    expect_error(R"({"@@extension_id":{"message":"fake"}})"sv, "\"@@extension_id\" is a reserved name"sv);
}

TEST_CASE(positional_substitution_edge_cases)
{
    Vector<String> values { "x"_string };
    EXPECT_EQ(format_message("a$xb"sv, {}, false).value(), "ab"sv);
    EXPECT_EQ(format_message("cost $$5"sv, {}, false).value(), "cost $5"sv);
    EXPECT_EQ(format_message("$$$1"sv, values, false).value(), "$$1"sv);
    EXPECT_EQ(format_message("$$$$"sv, {}, false).value(), "$$$"sv);
    EXPECT_EQ(format_message("$a$1$$"sv, values, false).value(), "x$"sv);
    EXPECT_EQ(format_message("$10"sv, values, false).value(), "x0"sv);
    EXPECT_EQ(format_message("$2"sv, values, false).value(), ""sv);
    EXPECT_EQ(format_message("end$"sv, {}, false).value(), "end"sv);
    EXPECT_EQ(format_message("$1"sv, {}, false).value(), ""sv);
    values[0] = "<value>"_string;
    EXPECT_EQ(format_message("<$1>"sv, values, true).value(), "&lt;<value>>"sv);
    EXPECT_EQ(format_message("$<"sv, {}, true).value(), "lt;"sv);
    values.resize(10);
    EXPECT(!format_message("anything"sv, values, false).has_value());
}

TEST_CASE(dollar_before_a_multibyte_character_leaves_valid_utf8)
{
    EXPECT_EQ(format_message("a$\xC3\xA9"
                             "b"sv,
                  {}, false)
                  .value(),
        "a\xEF\xBF\xBD"
        "b"sv);
}

TEST_CASE(msg_tokens_pass_other_bytes_through)
{
    HashMap<String, String> messages;
    messages.set("@@extension_id"_string, "id"_string);
    EXPECT_EQ(MUST(substitute_message_tokens("\xFF url(__MSG_@@extension_id__)"sv, messages, UnknownMessage::Keep)), "\xFF url(id)"sv);
}

TEST_CASE(msg_tokens_replace_and_skip_invalid_names)
{
    HashMap<String, String> messages;
    messages.set("name"_string, "__MSG_name__"_string);
    EXPECT_EQ(MUST(substitute_message_tokens("__MSG_NAME__ __MSG_bad-name__ __MSG_name__ end __MSG_"sv, messages, UnknownMessage::Fail)), "__MSG_name__ __MSG_bad-name__ __MSG_name__ end __MSG_"sv);
}

TEST_CASE(msg_tokens_unknown_fails_or_keeps)
{
    auto result = substitute_message_tokens("__MSG_missing__"sv, {}, UnknownMessage::Fail);
    EXPECT(result.is_error());
    if (result.is_error())
        EXPECT_EQ(result.error(), "\"__MSG_missing__\" isn't defined"sv);
    EXPECT_EQ(MUST(substitute_message_tokens("__MSG_missing__"sv, {}, UnknownMessage::Keep)), "__MSG_missing__"sv);
}

TEST_CASE(css_unknown_name_is_kept)
{
    HashMap<String, String> messages;
    messages.set("color"_string, "red"_string);
    EXPECT_EQ(MUST(substitute_message_tokens("/* __MSG_missing__ */ a { color: __MSG_COLOR__; }"sv, messages, UnknownMessage::Keep)), "/* __MSG_missing__ */ a { color: red; }"sv);
}

TEST_CASE(built_ins_follow_direction)
{
    auto rtl = built_in_messages("id"sv, "ar"sv);
    EXPECT_EQ(rtl.get("@@extension_id"sv).value(), "id"sv);
    EXPECT_EQ(rtl.get("@@ui_locale"sv).value(), "ar"sv);
    EXPECT_EQ(rtl.get("@@bidi_dir"sv).value(), "rtl"sv);
    EXPECT_EQ(rtl.get("@@bidi_reversed_dir"sv).value(), "ltr"sv);
    EXPECT_EQ(rtl.get("@@bidi_start_edge"sv).value(), "right"sv);
    EXPECT_EQ(rtl.get("@@bidi_end_edge"sv).value(), "left"sv);
    auto ltr = built_in_messages("id"sv, "en_US"sv);
    EXPECT_EQ(ltr.get("@@bidi_dir"sv).value(), "ltr"sv);
    EXPECT_EQ(ltr.get("@@bidi_reversed_dir"sv).value(), "rtl"sv);
    EXPECT_EQ(ltr.get("@@bidi_start_edge"sv).value(), "left"sv);
    EXPECT_EQ(ltr.get("@@bidi_end_edge"sv).value(), "right"sv);
}
