/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/GenericLexer.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16String.h>
#include <LibUnicode/Locale.h>
#include <LibWebCommon/Extensions/Localization.h>

namespace Web::Extensions {

bool is_valid_message_name(StringView name)
{
    return !name.is_empty() && all_of(name, [](auto ch) { return is_ascii_alphanumeric(ch) || ch == '_' || ch == '@'; });
}

static String invalid_name(StringView name)
{
    return MUST(String::formatted("\"{}\" isn't a valid name: only ASCII letters, digits, \"_\", and \"@\" are allowed", name));
}

static ErrorOr<ByteString, String> replace_variables(StringView text, HashMap<String, String> const& messages, StringView opening, StringView closing, UnknownMessage unknown)
{
    StringBuilder result;
    size_t copied = 0;
    size_t cursor = 0;
    while (true) {
        auto start = text.find(opening, cursor);
        if (!start.has_value())
            break;
        auto name_start = *start + opening.length();
        auto end = text.find(closing, name_start);
        if (!end.has_value())
            break;
        auto name = text.substring_view(name_start, *end - name_start);
        cursor = name_start;
        if (!is_valid_message_name(name))
            continue;
        cursor = *end + closing.length();
        auto value = messages.get(MUST(String::from_utf8(name)).to_ascii_lowercase());
        if (!value.has_value()) {
            auto error = MUST(String::formatted("\"{}{}{}\" isn't defined", opening, name, closing));
            if (unknown == UnknownMessage::Fail)
                return error;
            dbgln("{}", error);
            continue;
        }
        result.append(text.substring_view(copied, *start - copied));
        result.append(*value);
        copied = cursor;
    }
    result.append(text.substring_view(copied));
    return result.to_byte_string();
}

ErrorOr<HashMap<String, String>, String> parse_message_catalog(StringView json)
{
    auto parsed = JsonValue::from_string(json);
    if (parsed.is_error())
        return "not valid JSON"_string;
    if (!parsed.value().is_object())
        return "the top level must be an object"_string;
    HashMap<String, String> messages;
    TRY(parsed.value().as_object().try_for_each_member([&](String const& name, JsonValue const& value) -> ErrorOr<void, String> {
        auto key = name.to_ascii_lowercase();
        if (!is_valid_message_name(name))
            return invalid_name(name);
        if (key.is_one_of("@@extension_id"sv, "@@ui_locale"sv, "@@bidi_dir"sv, "@@bidi_reversed_dir"sv, "@@bidi_start_edge"sv, "@@bidi_end_edge"sv))
            return MUST(String::formatted("\"{}\" is a reserved name", name));
        if (!value.is_object())
            return MUST(String::formatted("\"{}\" must be an object", name));
        auto const& object = value.as_object();
        auto message = object.get_string("message"sv);
        if (!message.has_value())
            return MUST(String::formatted("\"{}\" needs a \"message\" string", name));
        HashMap<String, String> placeholders;
        if (auto values = object.get("placeholders"sv); values.has_value()) {
            if (!values->is_object())
                return MUST(String::formatted("\"{}\": \"placeholders\" must be an object", name));
            TRY(values->as_object().try_for_each_member([&](String const& placeholder_name, JsonValue const& placeholder_value) -> ErrorOr<void, String> {
                if (!is_valid_message_name(placeholder_name))
                    return invalid_name(placeholder_name);
                if (!placeholder_value.is_object())
                    return MUST(String::formatted("\"{}\": placeholder \"{}\" must be an object", name, placeholder_name));
                auto content = placeholder_value.as_object().get_string("content"sv);
                if (!content.has_value())
                    return MUST(String::formatted("\"{}\": placeholder \"{}\" needs a \"content\" string", name, placeholder_name));
                placeholders.set(placeholder_name.to_ascii_lowercase(), *content);
                return {};
            }));
        }
        auto replaced = replace_variables(*message, placeholders, "$"sv, "$"sv, UnknownMessage::Fail);
        if (replaced.is_error())
            return MUST(String::formatted("\"{}\": {}", name, replaced.error()));
        messages.set(move(key), MUST(String::from_byte_string(replaced.value())));
        return {};
    }));
    return messages;
}

// A locale's text runs right to left when its script does, whether the locale names that script or its likely subtags
// imply it. A locale whose script has no known direction is taken as left to right.
static bool is_locale_right_to_left(StringView locale)
{
    auto maximal = Unicode::add_likely_subtags(Utf16String::from_utf8(locale));
    if (!maximal.has_value())
        return false;
    auto locale_id = Unicode::parse_unicode_locale_id(maximal->utf16_view());
    if (!locale_id.has_value() || !locale_id->language_id.script.has_value())
        return false;
    return Unicode::is_script_right_to_left(*locale_id->language_id.script).value_or(false);
}

HashMap<String, String> built_in_messages(StringView extension_id, StringView ui_locale_underscored)
{
    auto locale = MUST(MUST(String::from_utf8(ui_locale_underscored)).replace("_"sv, "-"sv, ReplaceMode::All));
    auto rtl = is_locale_right_to_left(locale);
    HashMap<String, String> messages;
    messages.set("@@extension_id"_string, MUST(String::from_utf8(extension_id)));
    messages.set("@@ui_locale"_string, MUST(String::from_utf8(ui_locale_underscored)));
    messages.set("@@bidi_dir"_string, rtl ? "rtl"_string : "ltr"_string);
    messages.set("@@bidi_reversed_dir"_string, rtl ? "ltr"_string : "rtl"_string);
    messages.set("@@bidi_start_edge"_string, rtl ? "right"_string : "left"_string);
    messages.set("@@bidi_end_edge"_string, rtl ? "left"_string : "right"_string);
    return messages;
}

ErrorOr<ByteString, String> substitute_message_tokens(StringView text, HashMap<String, String> const& messages, UnknownMessage unknown)
{
    return replace_variables(text, messages, "__MSG_"sv, "__"sv, unknown);
}

Optional<String> format_message(StringView message, ReadonlySpan<String> substitutions, bool escape_lt)
{
    if (substitutions.size() > 9)
        return {};
    auto escaped = escape_lt ? MUST(MUST(String::from_utf8(message)).replace("<"sv, "&lt;"sv, ReplaceMode::All)) : MUST(String::from_utf8(message));
    message = escaped;
    StringBuilder result;
    GenericLexer lexer { message };
    while (!lexer.is_eof()) {
        result.append(lexer.consume_until('$'));
        // A run of dollar signs stands for one fewer of them. A lone one and the character after it stand for the
        // substitution that character numbers, if it numbers one, and otherwise for nothing.
        auto dollars = lexer.consume_while([](char ch) { return ch == '$'; });
        if (dollars.length() > 1) {
            result.append(dollars.substring_view(1));
            continue;
        }
        if (dollars.is_empty() || lexer.is_eof())
            break;
        auto index = lexer.consume();
        if (index >= '1' && index <= '9' && static_cast<size_t>(index - '1') < substitutions.size())
            result.append(substitutions[index - '1']);
    }
    // Dropping the character after a "$" removes only its first byte, so decode what's left of it as replacement
    // characters rather than keep invalid UTF-8.
    return String::from_utf8_with_replacement_character(result.string_view());
}

}
