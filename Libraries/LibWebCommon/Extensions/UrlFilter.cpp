/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Utf16String.h>
#include <LibRegex/ECMAScriptRegex.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/UrlFilter.h>

namespace Web::Extensions {

UrlFilter::UrlFilter() = default;
UrlFilter::UrlFilter(UrlFilter&&) = default;
UrlFilter& UrlFilter::operator=(UrlFilter&&) = default;
UrlFilter::~UrlFilter() = default;

struct StringKey {
    StringView name;
    u8 part;
    u8 test;
};

// Extensions compare these messages, so their wording is fixed.
static String expected(StringView key, StringView type, JsonValue const& value)
{
    return MUST(String::formatted("Error processing {}: Expected {} instead of {}", key, type, value.serialized()));
}

ErrorOr<UrlFilter, String> UrlFilter::parse(JsonObject const& object)
{
    using Part = Condition::Part;
    using Test = Condition::Test;
    static constexpr Array string_keys {
        StringKey { "hostContains"sv, to_underlying(Part::Host), to_underlying(Test::Contains) },
        StringKey { "hostEquals"sv, to_underlying(Part::Host), to_underlying(Test::Equals) },
        StringKey { "hostPrefix"sv, to_underlying(Part::Host), to_underlying(Test::Prefix) },
        StringKey { "hostSuffix"sv, to_underlying(Part::Host), to_underlying(Test::Suffix) },
        StringKey { "pathContains"sv, to_underlying(Part::Path), to_underlying(Test::Contains) },
        StringKey { "pathEquals"sv, to_underlying(Part::Path), to_underlying(Test::Equals) },
        StringKey { "pathPrefix"sv, to_underlying(Part::Path), to_underlying(Test::Prefix) },
        StringKey { "pathSuffix"sv, to_underlying(Part::Path), to_underlying(Test::Suffix) },
        StringKey { "queryContains"sv, to_underlying(Part::Query), to_underlying(Test::Contains) },
        StringKey { "queryEquals"sv, to_underlying(Part::Query), to_underlying(Test::Equals) },
        StringKey { "queryPrefix"sv, to_underlying(Part::Query), to_underlying(Test::Prefix) },
        StringKey { "querySuffix"sv, to_underlying(Part::Query), to_underlying(Test::Suffix) },
        StringKey { "urlContains"sv, to_underlying(Part::Url), to_underlying(Test::Contains) },
        StringKey { "urlEquals"sv, to_underlying(Part::Url), to_underlying(Test::Equals) },
        StringKey { "urlPrefix"sv, to_underlying(Part::Url), to_underlying(Test::Prefix) },
        StringKey { "urlSuffix"sv, to_underlying(Part::Url), to_underlying(Test::Suffix) },
    };

    UrlFilter filter;
    Optional<String> error;
    object.for_each_member([&](String const& key, JsonValue const& value) {
        if (error.has_value())
            return;
        StringKey const* string_key = nullptr;
        for (auto const& candidate : string_keys) {
            if (candidate.name == key)
                string_key = &candidate;
        }
        if (string_key) {
            if (!value.is_string()) {
                error = expected(key, "string"sv, value);
                return;
            }
            filter.m_conditions.append({ static_cast<Part>(string_key->part), static_cast<Test>(string_key->test), value.as_string() });
            return;
        }
        if (key.is_one_of("urlMatches"sv, "originAndPathMatches"sv)) {
            if (!value.is_string()) {
                error = expected(key, "string"sv, value);
                return;
            }
            auto compiled = regex::ECMAScriptRegex::compile(Utf16String::from_utf8(value.as_string()), {});
            if (compiled.is_error()) {
                error = MUST(String::formatted("Error processing {}: {}", key, compiled.error()));
                return;
            }
            auto regex = make<regex::ECMAScriptRegex>(compiled.release_value());
            if (key == "urlMatches"sv)
                filter.m_url_matches = move(regex);
            else
                filter.m_origin_and_path_matches = move(regex);
            return;
        }
        if (key == "schemes"sv) {
            if (!value.is_array()) {
                error = expected(key, "array"sv, value);
                return;
            }
            Vector<String> schemes;
            for (auto const& scheme : value.as_array().values()) {
                if (!scheme.is_string()) {
                    error = expected("schemes"sv, "string"sv, scheme);
                    return;
                }
                schemes.append(scheme.as_string());
            }
            filter.m_schemes = move(schemes);
            return;
        }
        if (key == "ports"sv) {
            if (!value.is_array()) {
                error = expected(key, "array"sv, value);
                return;
            }
            auto port_number = [](JsonValue const& port) -> Optional<u16> {
                auto number = port.get_u32();
                if (!number.has_value() || *number > 65535)
                    return {};
                return static_cast<u16>(*number);
            };
            Vector<Variant<u16, Array<u16, 2>>> ports;
            for (auto const& port : value.as_array().values()) {
                if (auto number = port_number(port); number.has_value()) {
                    ports.append(*number);
                    continue;
                }
                if (port.is_array() && port.as_array().size() == 2) {
                    auto from = port_number(port.as_array().at(0));
                    auto to = port_number(port.as_array().at(1));
                    if (from.has_value() && to.has_value()) {
                        ports.append(Array<u16, 2> { *from, *to });
                        continue;
                    }
                }
                error = expected("ports"sv, "integer or a [from, to] range"sv, port);
                return;
            }
            filter.m_ports = move(ports);
            return;
        }
        error = MUST(String::formatted("Unexpected property \"{}\"", key));
    });
    if (error.has_value())
        return error.release_value();
    return filter;
}

static bool regex_matches(regex::ECMAScriptRegex const& regex, String const& text)
{
    auto utf16 = Utf16String::from_utf8(text);
    return regex.test(utf16.utf16_view(), 0) == regex::MatchResult::Match;
}

bool UrlFilter::matches(URL::URL const& url) const
{
    if (m_schemes.has_value() && !m_schemes->contains_slow(url.scheme()))
        return false;

    if (m_ports.has_value()) {
        // A scheme without a default port gives a URL without one no port at all.
        auto port = url.port().has_value() ? Optional<u16> { *url.port() } : URL::default_port_for_scheme(url.scheme());
        if (!port.has_value())
            return false;
        auto port_matches = any_of(*m_ports, [&](auto const& entry) {
            return entry.visit(
                [&](u16 number) { return *port == number; },
                [&](Array<u16, 2> const& range) { return *port >= range[0] && *port <= range[1]; });
        });
        if (!port_matches)
            return false;
    }

    // An IPv6 host is compared without the brackets it's serialized with.
    auto host_view = url.host().has_value() ? url.serialized_host() : StringView {};
    if (host_view.starts_with('[') && host_view.ends_with(']'))
        host_view = host_view.substring_view(1, host_view.length() - 2);
    auto host = MUST(String::from_utf8(host_view));
    auto path = MUST(String::from_utf8(url.serialize_path()));
    auto query = MUST(String::from_utf8(url.query().value_or({})));
    auto whole = url.serialize();
    for (auto const& condition : m_conditions) {
        String const* part = nullptr;
        switch (condition.part) {
        case Condition::Part::Host:
            part = &host;
            break;
        case Condition::Part::Path:
            part = &path;
            break;
        case Condition::Part::Query:
            part = &query;
            break;
        case Condition::Part::Url:
            part = &whole;
            break;
        }
        // An empty Contains, Prefix, or Suffix condition is no condition; otherwise an empty part only equals.
        if (condition.test != Condition::Test::Equals && condition.value.is_empty())
            continue;
        if (condition.test != Condition::Test::Equals && part->is_empty())
            return false;
        bool matched = false;
        switch (condition.test) {
        case Condition::Test::Contains:
            // The host is tested with a dot in front, so a leading dot in the filter finds a whole label.
            matched = condition.part == Condition::Part::Host
                ? MUST(String::formatted(".{}", *part)).contains(condition.value)
                : part->contains(condition.value);
            break;
        case Condition::Test::Equals:
            matched = *part == condition.value;
            break;
        case Condition::Test::Prefix:
            matched = part->starts_with_bytes(condition.value);
            break;
        case Condition::Test::Suffix:
            matched = part->ends_with_bytes(condition.value);
            break;
        }
        if (!matched)
            return false;
    }

    if (m_url_matches && !regex_matches(*m_url_matches, url.serialize(URL::ExcludeFragment::Yes)))
        return false;
    if (m_origin_and_path_matches) {
        auto origin_and_path = url;
        origin_and_path.set_query({});
        if (!regex_matches(*m_origin_and_path_matches, origin_and_path.serialize(URL::ExcludeFragment::Yes)))
            return false;
    }
    return true;
}

}
