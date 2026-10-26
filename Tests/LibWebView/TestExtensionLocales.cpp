/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>

struct LocaleFixture {
    LocaleFixture()
        : path(LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-extension-locales-{}-{}", Core::System::getpid(), ++counter)).string())
        , settings(WebView::Settings::create(LexicalPath::join(path, "Settings.json"sv).string(), { "fr-CA"_string }))
        , registry(settings)
    {
        mkdir(""sv);
        manifest(R"("default_locale":"en")"sv);
    }
    ~LocaleFixture() { MUST(FileSystem::remove(path, FileSystem::RecursionMode::Allowed)); }
    void mkdir(StringView relative) { MUST(Core::Directory::create(LexicalPath::join(path, relative), Core::Directory::CreateDirectories::Yes)); }
    void write(StringView relative, StringView contents)
    {
        auto full_path = LexicalPath::join(path, relative);
        MUST(Core::Directory::create(full_path.dirname(), Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(full_path.string(), Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(contents.bytes()));
    }
    void manifest(StringView extra, StringView name = "Minimal"sv)
    {
        write("manifest.json"sv, ByteString::formatted(R"({{"manifest_version":3,"name":"{}","version":"1"{}}})", name, extra.is_empty() ? ByteString {} : ByteString::formatted(",{}", extra)));
    }
    void catalog(StringView locale, StringView contents) { write(ByteString::formatted("_locales/{}/messages.json", locale), contents); }
    void expect_error(StringView message)
    {
        auto result = registry.load_unpacked(path);
        EXPECT(result.is_error());
        if (result.is_error())
            EXPECT_EQ(result.error(), message);
        EXPECT(settings.installed_extensions().is_empty());
        EXPECT(registry.enabled_descriptors().is_empty());
    }
    void expect_error_prefix(StringView prefix)
    {
        auto result = registry.load_unpacked(path);
        EXPECT(result.is_error());
        if (result.is_error())
            EXPECT(result.error().starts_with_bytes(prefix));
        EXPECT(settings.installed_extensions().is_empty());
    }
    ByteString path;
    WebView::Settings settings;
    WebView::ExtensionRegistry registry;
    static inline size_t counter = 0;
};

TEST_CASE(manifest_name_is_localized)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"en","description":"__MSG_desc__","short_name":"__MSG_title__","action":{"default_title":"__MSG_title__"},"browser_action":{"default_title":"__MSG_title__"},"page_action":{"default_title":"__MSG_title__"})"sv, "__MSG_title__"sv);
    fixture.catalog("en"sv, R"({"title":{"message":"Localized"},"desc":{"message":"Description"}})"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().name, "Localized"sv);
    auto json = MUST(JsonValue::from_string(extension->descriptor().manifest_json));
    EXPECT_EQ(json.as_object().get_string("description"sv).value(), "Description"sv);
    EXPECT_EQ(json.as_object().get_string("short_name"sv).value(), "Localized"sv);
    for (auto key : { "action"sv, "browser_action"sv, "page_action"sv })
        EXPECT_EQ(json.as_object().get_object(key)->get_string("default_title"sv).value(), "Localized"sv);
}

TEST_CASE(non_localized_fields_keep_their_tokens)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"en","content_scripts":[{"matches":["<all_urls>"],"js":["__MSG_script__"]}])"sv);
    fixture.catalog("en"sv, R"({"script":{"message":"script.js"}})"sv);
    fixture.write("script.js"sv, "// script"sv);
    auto result = fixture.registry.load_unpacked(fixture.path);
    EXPECT(result.is_error());
    if (result.is_error())
        EXPECT(result.error().contains("__MSG_script__"sv));
    EXPECT(fixture.settings.installed_extensions().is_empty());
}

TEST_CASE(fallback_chain_uses_language_prefix)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"EN","description":"__MSG_desc__")"sv, "__MSG_title__"sv);
    fixture.catalog("eN"sv, R"({"title":{"message":"English"},"desc":{"message":"Default"}})"sv);
    fixture.catalog("Fr"sv, R"({"title":{"message":"French"}})"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().name, "French"sv);
    auto json = MUST(JsonValue::from_string(extension->descriptor().manifest_json));
    EXPECT_EQ(json.as_object().get_string("description"sv).value(), "Default"sv);
}

TEST_CASE(default_locale_wins_when_nothing_else_matches)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"en")"sv, "__MSG_title__"sv);
    fixture.catalog("en"sv, R"({"title":{"message":"English"}})"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().name, "English"sv);
}

TEST_CASE(css_built_ins_without_locales)
{
    LocaleFixture fixture;
    fixture.manifest(R"("content_scripts":[{"matches":["<all_urls>"],"css":["style.css"]}])"sv);
    fixture.write("style.css"sv, "a { background: url(ladybird-extension://__MSG_@@extension_id__/img.png); content: '__MSG_@@ui_locale__'; }"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().content_scripts[0].css_sources[0], MUST(String::formatted("a {{ background: url(ladybird-extension://{}/img.png); content: 'fr_CA'; }}", extension->id())));
}

TEST_CASE(content_script_css_is_localized)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"en","content_scripts":[{"matches":["<all_urls>"],"css":["style.css"]}])"sv);
    fixture.catalog("en"sv, R"({"color":{"message":"red"}})"sv);
    fixture.write("style.css"sv, "a { color: __MSG_color__; }"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().content_scripts[0].css_sources[0], "a { color: red; }"sv);
}

TEST_CASE(default_locale_must_be_a_string)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":42)"sv);
    fixture.expect_error("manifest.json: \"default_locale\" must be a string"sv);
}

TEST_CASE(locales_require_a_default)
{
    for (auto extra : { ""sv, R"("default_locale":"")"sv, R"("default_locale":"missing")"sv }) {
        LocaleFixture fixture;
        fixture.manifest(extra);
        fixture.catalog("en"sv, "{}"sv);
        fixture.expect_error("manifest.json: \"default_locale\" must name a locale in _locales"sv);
    }
}

TEST_CASE(default_locale_requires_locales_subtree)
{
    LocaleFixture fixture;
    fixture.expect_error("manifest.json: \"default_locale\" is set, but there's no _locales directory"sv);
}

TEST_CASE(default_locale_requires_readable_catalog)
{
    LocaleFixture fixture;
    fixture.mkdir("_locales/en"sv);
    fixture.expect_error_prefix("Can't read \"_locales/en/messages.json\": "sv);
}

TEST_CASE(other_locale_requires_catalog)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, "{}"sv);
    fixture.mkdir("_locales/de"sv);
    fixture.expect_error_prefix("Can't read \"_locales/de/messages.json\": "sv);
}

TEST_CASE(reserved_key_fails_the_load)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, R"({"@@ui_locale":{"message":"fake"}})"sv);
    fixture.expect_error("_locales/en/messages.json: \"@@ui_locale\" is a reserved name"sv);
}

TEST_CASE(undefined_placeholder_fails_the_load)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, R"({"hello":{"message":"$who$"}})"sv);
    fixture.expect_error("_locales/en/messages.json: \"hello\": \"$who$\" isn't defined"sv);
}

TEST_CASE(undefined_manifest_message_fails_the_load)
{
    LocaleFixture fixture;
    fixture.manifest(R"("default_locale":"en")"sv, "__MSG_missing__"sv);
    fixture.catalog("en"sv, "{}"sv);
    fixture.expect_error("manifest.json: \"__MSG_missing__\" isn't defined"sv);
}

TEST_CASE(invalid_unselected_catalog_fails_the_load)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, "{}"sv);
    fixture.catalog("de"sv, "[]"sv);
    fixture.expect_error("_locales/de/messages.json: the top level must be an object"sv);
}

TEST_CASE(invalid_locale_directory_is_ignored)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, "{}"sv);
    fixture.mkdir("_locales/not_a_locale!"sv);
    EXPECT(!fixture.registry.load_unpacked(fixture.path).is_error());
}

TEST_CASE(locale_snapshot_changes_only_on_reload)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, R"({"title":{"message":"English"}})"sv);
    fixture.catalog("fr"sv, R"({"title":{"message":"French"}})"sv);
    fixture.manifest(R"("default_locale":"en")"sv, "__MSG_title__"sv);
    auto first = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(first->descriptor().ui_locale, "fr-CA"sv);
    EXPECT_EQ(first->descriptor().messages.get("@@ui_locale"sv).value(), "fr_CA"sv);
    fixture.settings.set_languages({ "en-US"_string });
    EXPECT_EQ(first->descriptor().name, "French"sv);
    auto second = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(second->id(), first->id());
    EXPECT_EQ(second->descriptor().name, "English"sv);
    EXPECT_EQ(second->descriptor().ui_locale, "en-US"sv);
    EXPECT_EQ(first->descriptor().name, "French"sv);
}

TEST_CASE(regional_locale_wins_per_message)
{
    LocaleFixture fixture;
    fixture.catalog("en"sv, R"({"title":{"message":"English"},"desc":{"message":"Default"}})"sv);
    fixture.catalog("fr"sv, R"({"title":{"message":"French"},"desc":{"message":"Parent"}})"sv);
    fixture.catalog("fr_CA"sv, R"({"title":{"message":"Regional"}})"sv);
    fixture.manifest(R"("default_locale":"en","description":"__MSG_desc__")"sv, "__MSG_title__"sv);
    auto extension = TRY_OR_FAIL(fixture.registry.load_unpacked(fixture.path));
    EXPECT_EQ(extension->descriptor().name, "Regional"sv);
    EXPECT_EQ(extension->descriptor().messages.get("desc"sv).value(), "Parent"sv);
}
