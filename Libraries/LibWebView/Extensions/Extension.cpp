/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Utf16View.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibWebCommon/Extensions/Localization.h>
#include <LibWebCommon/Extensions/ResourcePath.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/Extensions/Extension.h>
#include <limits.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#else
#    include <unistd.h>
#endif

namespace WebView {

static ErrorOr<ByteString> opened_file_path(int fd)
{
#if defined(AK_OS_MACOS)
    char path[PATH_MAX];
    TRY(Core::System::fcntl(fd, F_GETPATH, path));
    return ByteString(path);
#elif defined(AK_OS_WINDOWS)
    auto length = GetFinalPathNameByHandleW(to_handle(fd), nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length)
        return Error::from_windows_error();
    Vector<wchar_t> path;
    TRY(path.try_resize(length));
    auto result = GetFinalPathNameByHandleW(to_handle(fd), path.data(), length, FILE_NAME_NORMALIZED);
    if (!result || result >= length)
        return Error::from_windows_error();
    auto string = TRY((Utf16View { reinterpret_cast<char16_t const*>(path.data()), result }.to_utf8()));
    return string.to_byte_string();
#else
    auto link = ByteString::formatted("/proc/self/fd/{}", fd);
    char path[PATH_MAX];
    auto length = readlink(link.characters(), path, sizeof(path));
    if (length < 0)
        return Error::from_errno(errno);
    if (static_cast<size_t>(length) == sizeof(path))
        return Error::from_errno(ENAMETOOLONG);
    return ByteString(path, length);
#endif
}

static ErrorOr<NonnullOwnPtr<Core::File>> open_resource_file(Core::Directory const& directory, StringView url_path)
{
    auto relative_path = TRY(Web::Extensions::canonical_resource_path(url_path));
    auto mode = Core::File::OpenMode::Read;
#ifndef AK_OS_WINDOWS
    mode |= Core::File::OpenMode::Nonblocking;
#endif
    auto file = TRY(directory.open(relative_path, mode));
    if (!S_ISREG(TRY(file->stat()).st_mode))
        return Error::from_errno(EACCES);

    // Check the opened file, so replacing a path with a symlink can't change the file after authorization.
    auto root = TRY(opened_file_path(directory.fd()));
    auto path = TRY(opened_file_path(file->fd()));
#ifdef AK_OS_WINDOWS
    auto prefix = ByteString::formatted("{}\\", root);
    if (!path.starts_with(prefix, CaseSensitivity::CaseInsensitive))
#else
    auto prefix = ByteString::formatted("{}/", root);
    if (!path.starts_with(prefix))
#endif
        return Error::from_errno(EACCES);

#ifndef AK_OS_WINDOWS
    TRY(file->set_blocking(true));
#endif
    return file;
}

static ErrorOr<String> read_file_as_utf8(Core::Directory const& directory, StringView relative_path)
{
    auto path = ByteString::formatted("/{}", relative_path.trim("/"sv, TrimMode::Left));
    auto file = TRY(open_resource_file(directory, path));
    auto contents = TRY(file->read_until_eof());
    auto string = String::from_utf8(contents);
    if (string.is_error())
        return Error::from_string_literal("not valid UTF-8");
    return string.release_value();
}

static String read_error(StringView relative_path, Error const& error)
{
    return MUST(String::formatted("Can't read \"{}\": {}", relative_path, error));
}

static bool is_locale_directory(StringView name)
{
    auto parts = name.split_view('_', SplitBehavior::KeepEmpty);
    if (parts.is_empty() || parts[0].length() < 2 || parts[0].length() > 3)
        return false;
    if (!all_of(parts[0], [](auto ch) { return is_ascii_alpha(ch); }))
        return false;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].length() < 2 || parts[i].length() > 8 || !all_of(parts[i], [](auto ch) { return is_ascii_alphanumeric(ch); }))
            return false;
    }
    return true;
}

static String normalized_locale(String const& locale)
{
    return MUST(locale.replace("-"sv, "_"sv, ReplaceMode::All)).to_ascii_lowercase();
}

static ErrorOr<void, String> localize_manifest_field(JsonObject& object, StringView key, HashMap<String, String> const& messages)
{
    auto value = object.get_string(key);
    if (!value.has_value())
        return {};
    auto localized = Web::Extensions::substitute_message_tokens(*value, messages, Web::Extensions::UnknownMessage::Fail);
    if (localized.is_error())
        return MUST(String::formatted("manifest.json: {}", localized.error()));
    object.set(key, MUST(String::from_byte_string(localized.release_value())));
    return {};
}

static ErrorOr<HashMap<String, String>, String> load_localization(Core::Directory const& directory, JsonObject& manifest, String const& id, String const& locale)
{
    auto messages = Web::Extensions::built_in_messages(id, MUST(locale.replace("-"sv, "_"sv, ReplaceMode::All)));
    auto default_value = manifest.get("default_locale"sv);
    if (default_value.has_value() && !default_value->is_string())
        return "manifest.json: \"default_locale\" must be a string"_string;
    auto default_locale = normalized_locale(manifest.get_string("default_locale"sv).value_or({}));
    auto locales_stat = directory.stat("_locales"sv);
    if (locales_stat.is_error() || !S_ISDIR(locales_stat.value().st_mode)) {
        if (!default_locale.is_empty())
            return "manifest.json: \"default_locale\" is set, but there's no _locales directory"_string;
        return messages;
    }
    HashMap<String, String> directories;
    Core::DirIterator entries(LexicalPath::join(directory.path().string(), "_locales"sv).string(), Core::DirIterator::SkipDots);
    Vector<ByteString> names;
    while (entries.has_next())
        names.append(entries.next_path());
    if (entries.has_error())
        return read_error("_locales"sv, entries.error());
    quick_sort(names);
    for (auto const& name : names) {
        if (!is_locale_directory(name))
            continue;
        auto stat = directory.stat(ByteString::formatted("_locales/{}", name));
        if (stat.is_error() || !S_ISDIR(stat.value().st_mode))
            continue;
        auto spelling = MUST(String::from_utf8(name.view()));
        auto normalized = normalized_locale(spelling);
        if (auto other = directories.get(normalized); other.has_value())
            return MUST(String::formatted("_locales/{} and _locales/{} are the same locale", *other, name));
        directories.set(move(normalized), move(spelling));
    }
    if (default_locale.is_empty() || !directories.contains(default_locale))
        return "manifest.json: \"default_locale\" must name a locale in _locales"_string;

    Vector<String> fallback;
    auto candidate = normalized_locale(locale);
    while (!candidate.is_empty()) {
        if (!fallback.contains_slow(candidate))
            fallback.append(candidate);
        auto separator = candidate.bytes_as_string_view().find_last('_');
        if (!separator.has_value())
            break;
        candidate = MUST(candidate.substring_from_byte_offset(0, *separator));
    }
    if (!fallback.contains_slow(default_locale))
        fallback.append(default_locale);

    HashMap<String, HashMap<String, String>> catalogs;
    for (auto const& entry : directories) {
        auto path = MUST(String::formatted("_locales/{}/messages.json", entry.value));
        auto source = read_file_as_utf8(directory, path);
        if (source.is_error())
            return read_error(path, source.error());
        auto parsed = Web::Extensions::parse_message_catalog(source.value());
        if (parsed.is_error())
            return MUST(String::formatted("{}: {}", path, parsed.error()));
        if (fallback.contains_slow(entry.key))
            catalogs.set(entry.key, parsed.release_value());
    }
    for (auto const& tag : fallback) {
        auto catalog = catalogs.find(tag);
        if (catalog == catalogs.end())
            continue;
        for (auto& entry : catalog->value) {
            if (!messages.contains(entry.key))
                messages.set(entry.key, move(entry.value));
        }
    }
    for (auto key : { "name"sv, "short_name"sv, "description"sv })
        TRY(localize_manifest_field(manifest, key, messages));
    for (auto key : { "action"sv, "browser_action"sv, "page_action"sv }) {
        if (auto action = manifest.get_object(key); action.has_value())
            TRY(localize_manifest_field(*action, "default_title"sv, messages));
    }
    return messages;
}

void Extension::apply_permission_grants(Vector<String> const& grants)
{
    m_descriptor.api_permissions.clear();
    for (auto const& permission : m_manifest.permissions) {
        if (!grants.contains_slow(permission)) {
            dbgln("Extension {} requests an ungranted permission: {}", m_id, permission);
            continue;
        }
        if (!m_descriptor.api_permissions.contains_slow(permission))
            m_descriptor.api_permissions.append(permission);
    }
    // An optional permission or host pattern counts only once it has been granted.
    for (auto const& permission : m_manifest.optional_permissions) {
        if (grants.contains_slow(permission) && !m_descriptor.api_permissions.contains_slow(permission))
            m_descriptor.api_permissions.append(permission);
    }
    quick_sort(m_descriptor.api_permissions);
    m_descriptor.granted_origins.clear();
    for (size_t i = 0; i < m_manifest.optional_host_permissions.size(); ++i) {
        if (grants.contains_slow(m_manifest.optional_host_permissions[i]))
            m_descriptor.granted_origins.append(m_descriptor.optional_host_permissions[i]);
    }
}

ErrorOr<NonnullRefPtr<Extension>, String> Extension::load_unpacked(ByteString directory, String id, Optional<Vector<String>> granted_permissions, String locale)
{
    auto resource_directory = Core::Directory::create(directory, Core::Directory::CreateDirectories::No);
    if (resource_directory.is_error())
        return read_error("manifest.json"sv, resource_directory.error());
    auto manifest_json = read_file_as_utf8(resource_directory.value(), "manifest.json"sv);
    if (manifest_json.is_error())
        return read_error("manifest.json"sv, manifest_json.error());
    auto json = JsonValue::from_string(manifest_json.value());
    if (json.is_error())
        return "manifest.json: not valid JSON"_string;
    if (!json.value().is_object())
        return "manifest.json: the top level must be an object"_string;
    if (locale.is_empty())
        locale = Web::default_preferred_languages().first();
    locale = MUST(locale.replace("_"sv, "-"sv, ReplaceMode::All));
    auto messages = TRY(load_localization(resource_directory.value(), json.value().as_object(), id, locale));
    auto localized_manifest = json.value().serialized();
    auto manifest = TRY(ExtensionManifest::parse(localized_manifest));

    Web::Extensions::ExtensionDescriptor descriptor;
    descriptor.id = id;
    descriptor.name = manifest.name;
    descriptor.version = manifest.version;
    descriptor.manifest_version = manifest.manifest_version;
    descriptor.manifest_json = move(localized_manifest);
    descriptor.messages = move(messages);
    descriptor.ui_locale = move(locale);
    for (auto const& host : manifest.host_permissions) {
        auto pattern = Web::Extensions::MatchPattern::parse(host);
        if (pattern.is_error())
            return MUST(String::formatted("manifest.json: Invalid host permission: {}", host));
        descriptor.host_permissions.append(pattern.release_value());
    }
    descriptor.optional_api_permissions = manifest.optional_permissions;
    for (auto const& host : manifest.optional_host_permissions) {
        auto pattern = Web::Extensions::MatchPattern::parse(host);
        if (pattern.is_error())
            return MUST(String::formatted("manifest.json: Invalid optional host permission: {}", host));
        descriptor.optional_host_permissions.append(pattern.release_value());
    }
    descriptor.extension_pages_csp = manifest.extension_pages_csp;
    descriptor.web_accessible_resources = manifest.web_accessible_resources;
    descriptor.has_background = manifest.background.has_value();
    descriptor.has_action = manifest.action.has_value();
    descriptor.background_document_html = manifest.generated_background_document_html();

    for (auto const& script : manifest.content_scripts) {
        auto with_sources = script;
        for (auto const& path : script.js_paths) {
            auto source = read_file_as_utf8(resource_directory.value(), path);
            if (source.is_error())
                return read_error(path, source.error());
            with_sources.js_sources.append(source.release_value());
        }
        for (auto const& path : script.css_paths) {
            auto source = read_file_as_utf8(resource_directory.value(), path);
            if (source.is_error())
                return read_error(path, source.error());
            with_sources.css_sources.append(MUST(String::from_byte_string(MUST(Web::Extensions::substitute_message_tokens(source.value(), descriptor.messages, Web::Extensions::UnknownMessage::Keep)))));
        }
        descriptor.content_scripts.append(move(with_sources));
    }

    auto extension = adopt_ref(*new Extension(move(directory), resource_directory.release_value(), move(id), move(manifest), move(descriptor)));
    extension->apply_permission_grants(granted_permissions.value_or(extension->manifest().permissions));
    return extension;
}

Extension::Extension(ByteString directory, Core::Directory resource_directory, String id, ExtensionManifest manifest, Web::Extensions::ExtensionDescriptor descriptor)
    : m_directory(move(directory))
    , m_resource_directory(move(resource_directory))
    , m_id(move(id))
    , m_manifest(move(manifest))
    , m_descriptor(move(descriptor))
{
    renew_instance();
}

void Extension::renew_instance()
{
    static u64 revision = 0;
    m_descriptor.revision = ++revision;
}

bool Extension::is_web_accessible(StringView url_path, URL::URL const& initiator) const
{
    return m_descriptor.is_web_accessible(url_path, initiator);
}

ErrorOr<ByteString> Extension::resolve_resource_path(StringView url_path) const
{
    auto relative_path = TRY(Web::Extensions::canonical_resource_path(url_path));
    return LexicalPath::join(m_directory, relative_path).string();
}

ErrorOr<NonnullOwnPtr<Core::File>> Extension::open_resource(StringView url_path) const
{
    return open_resource_file(m_resource_directory, url_path);
}

}
