/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebView/Extensions/ExtensionTabsAPI.h>

namespace WebView {

ErrorOr<URL::URL, String> resolve_extension_tab_url(Web::Extensions::ExtensionDescriptor const& extension, StringView input)
{
    auto base = URL::Parser::basic_parse(MUST(String::formatted("{}://{}/", Web::Extensions::scheme, extension.id)));
    VERIFY(base.has_value());
    auto url = URL::Parser::basic_parse(input, *base);
    if (!url.has_value())
        return MUST(String::formatted("Invalid url: \"{}\".", input));
    if (url->scheme() == "javascript"sv)
        return "JavaScript URLs are not allowed in API based extension navigations. Use chrome.scripting.executeScript instead."_string;
    if (url->scheme() == "file"sv)
        return "Cannot navigate to a file URL without local file access."_string;
    if (url->scheme() == Web::Extensions::scheme && url->serialized_host() != extension.id)
        return "Cannot navigate to another extension's page."_string;
    return url.release_value();
}

ErrorOr<ExtensionTabCreation, String> parse_extension_tab_creation(Web::Extensions::ExtensionDescriptor const& extension, JsonObject const& properties)
{
    ExtensionTabCreation creation;
    if (auto url = properties.get_string("url"sv); url.has_value())
        creation.url = TRY(resolve_extension_tab_url(extension, *url));
    creation.active = properties.get_bool("active"sv).value_or(true);
    creation.index = properties.get_integer<i64>("index"sv);
    creation.window_id = properties.get_integer<i64>("windowId"sv);
    creation.pinned = properties.get_bool("pinned"sv).value_or(false);
    return creation;
}

ErrorOr<ExtensionTabUpdate, String> parse_extension_tab_update(Web::Extensions::ExtensionDescriptor const& extension, JsonObject const& properties)
{
    ExtensionTabUpdate update;
    if (auto url = properties.get_string("url"sv); url.has_value())
        update.url = TRY(resolve_extension_tab_url(extension, *url));
    update.active = properties.get_bool("active"sv);
    update.pinned = properties.get_bool("pinned"sv);
    return update;
}

}
