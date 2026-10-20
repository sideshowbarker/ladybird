/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Utf16View.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibWebCommon/Extensions/ResourcePath.h>
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

ErrorOr<NonnullRefPtr<Extension>, String> Extension::load_unpacked(ByteString directory, String id)
{
    auto resource_directory = Core::Directory::create(directory, Core::Directory::CreateDirectories::No);
    if (resource_directory.is_error())
        return read_error("manifest.json"sv, resource_directory.error());
    auto manifest_json = read_file_as_utf8(resource_directory.value(), "manifest.json"sv);
    if (manifest_json.is_error())
        return read_error("manifest.json"sv, manifest_json.error());
    auto manifest = TRY(ExtensionManifest::parse(manifest_json.value()));

    Web::Extensions::ExtensionDescriptor descriptor;
    descriptor.id = id;
    descriptor.name = manifest.name;
    descriptor.version = manifest.version;
    descriptor.manifest_version = manifest.manifest_version;
    descriptor.manifest_json = manifest_json.release_value();
    descriptor.extension_pages_csp = manifest.extension_pages_csp;
    descriptor.web_accessible_resources = manifest.web_accessible_resources;
    descriptor.has_background = manifest.background.has_value();
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
            with_sources.css_sources.append(source.release_value());
        }
        descriptor.content_scripts.append(move(with_sources));
    }

    return adopt_ref(*new Extension(move(directory), resource_directory.release_value(), move(id), move(manifest), move(descriptor)));
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
