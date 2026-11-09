/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ResourcePath.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/Extensions/ExtensionArchive.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionRegisteredScripts.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>
#include <LibWebView/Settings.h>

namespace WebView {

ExtensionRegistryObserver::ExtensionRegistryObserver(ExtensionRegistry& registry)
    : m_registry(registry)
{
    registry.m_observers.append(make_weak_ptr());
}

ExtensionRegistryObserver::~ExtensionRegistryObserver()
{
    if (m_registry)
        m_registry->m_observers.remove_all_matching([&](auto const& observer) { return observer.ptr() == this; });
}

ExtensionRegistry::ExtensionRegistry(Settings& settings)
    : m_settings(settings)
{
}

// Extensions are localized for the first language the user prefers.
static String extension_locale(Settings const& settings)
{
    auto const& languages = settings.languages();
    return languages.is_empty() ? Web::default_preferred_languages().first() : languages.first();
}

ErrorOr<PendingExtensionInstall, String> ExtensionRegistry::prepare_unpacked(ByteString directory)
{
    auto canonical_directory = FileSystem::real_path(directory);
    if (canonical_directory.is_error())
        return MUST(String::formatted("Can't resolve extension directory: {}", canonical_directory.error()));
    directory = canonical_directory.release_value();
    Optional<String> existing_id;
    Vector<String> granted_permissions;
    for (auto const& installed : m_settings.installed_extensions()) {
        auto installed_directory = FileSystem::real_path(installed.path);
        if (installed.path == directory || (!installed_directory.is_error() && installed_directory.value() == directory)) {
            existing_id = installed.id;
            granted_permissions = installed.granted_permissions;
            break;
        }
    }

    auto id = existing_id.value_or_lazy_evaluated([&] { return AK::generate_random_uuid(); });
    auto extension = TRY(Extension::load_unpacked(directory, id, {}, extension_locale(m_settings)));
    Vector<String> newly_declared_permissions;
    for (auto const& permission : extension->descriptor().api_permissions) {
        if (!granted_permissions.contains_slow(permission))
            newly_declared_permissions.append(permission);
    }
    return PendingExtensionInstall { move(extension), existing_id.has_value(), move(newly_declared_permissions) };
}

static constexpr auto staging_prefix = ".staging-"sv;
static constexpr auto backup_prefix = ".backup-"sv;

ByteString ExtensionRegistry::extensions_directory() const
{
    return LexicalPath::join(m_profile_data_directory.value_or(Core::StandardPaths::tempfile_directory()), "extensions"sv).string();
}

// At startup, a staged package no install took (left by a crash) is removed, and an extension whose reinstall was
// interrupted gets back the package it had, from the backup set aside for it.
void ExtensionRegistry::recover_interrupted_installs() const
{
    auto directory = extensions_directory();
    if (!FileSystem::exists(directory))
        return;
    Vector<ByteString> extension_directories;
    (void)Core::Directory::for_each_entry(directory, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const&) {
        auto path = LexicalPath::join(directory, entry.name).string();
        if (entry.name.starts_with(staging_prefix))
            (void)remove_extension_package(path);
        else if (FileSystem::is_directory(path))
            extension_directories.append(path);
        return IterationDecision::Continue;
    });
    for (auto const& extension_directory : extension_directories) {
        auto package = LexicalPath::join(extension_directory, "package"sv).string();
        Vector<ByteString> backups;
        (void)Core::Directory::for_each_entry(extension_directory, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const&) {
            if (entry.name.starts_with(backup_prefix))
                backups.append(LexicalPath::join(extension_directory, entry.name).string());
            return IterationDecision::Continue;
        });
        for (auto const& backup : backups) {
            if (!FileSystem::exists(package) && !FileSystem::rename_without_replacing(package, backup).is_error())
                continue;
            (void)remove_extension_package(backup);
        }
    }
}

ErrorOr<PendingExtensionInstall, String> ExtensionRegistry::prepare_packed(ByteString archive_path)
{
    auto file = Core::File::open(archive_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return MUST(String::formatted("Can't open the extension archive: {}", file.error()));
    // An archive too large to hold an extension isn't read into memory at all.
    auto size = FileSystem::size_from_stat(archive_path);
    if (size.is_error() || static_cast<u64>(size.value()) > extension_archive_max_file_size)
        return "The extension archive is too large to install"_string;
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return MUST(String::formatted("Can't read the extension archive: {}", bytes.error()));
    auto staging = LexicalPath::join(extensions_directory(), ByteString::formatted("{}{}", staging_prefix, AK::generate_random_uuid())).string();
    TRY(extract_extension_archive(bytes.value(), staging));
    ArmedScopeGuard remove_staging = [&] { (void)remove_extension_package(staging); };

    auto manifest_file = Core::File::open(LexicalPath::join(staging, "manifest.json"sv).string(), Core::File::OpenMode::Read);
    if (manifest_file.is_error())
        return "The extension archive has no manifest.json"_string;
    auto manifest_json = manifest_file.value()->read_until_eof();
    auto manifest = manifest_json.is_error() ? ErrorOr<ExtensionManifest, String> { "Can't read manifest.json"_string } : ExtensionManifest::parse(StringView { manifest_json.value() });
    if (manifest.is_error())
        return manifest.release_error();

    // An archive with a Gecko id replaces the packed extension that has it.
    Optional<String> existing_id;
    Vector<String> granted_permissions;
    if (auto const& gecko_id = manifest.value().gecko_id; gecko_id.has_value()) {
        for (auto const& installed : m_settings.installed_extensions()) {
            if (installed.packed && installed.gecko_id == gecko_id) {
                existing_id = installed.id;
                granted_permissions = installed.granted_permissions;
                break;
            }
        }
    }
    auto id = existing_id.value_or_lazy_evaluated([&] { return AK::generate_random_uuid(); });
    auto extension = TRY(Extension::load_unpacked(staging, id, {}, extension_locale(m_settings)));
    Vector<String> newly_declared_permissions;
    for (auto const& permission : extension->descriptor().api_permissions) {
        if (!granted_permissions.contains_slow(permission))
            newly_declared_permissions.append(permission);
    }
    Optional<String> replaces;
    if (existing_id.has_value()) {
        auto installed = find(*existing_id);
        replaces = installed ? installed->manifest().name : extension->manifest().name;
    }
    remove_staging.disarm();
    return PendingExtensionInstall { move(extension), existing_id.has_value(), move(newly_declared_permissions), move(staging), move(replaces) };
}

bool install_needs_confirmation(PendingExtensionInstall const& pending)
{
    return !pending.newly_declared_permissions.is_empty() || !pending.extension->descriptor().host_permissions.is_empty() || pending.replaces.has_value();
}

void ExtensionRegistry::discard(PendingExtensionInstall const& pending)
{
    if (pending.staged_package.has_value() && FileSystem::exists(*pending.staged_package))
        (void)remove_extension_package(*pending.staged_package);
}

NonnullRefPtr<Extension> ExtensionRegistry::install(PendingExtensionInstall pending)
{
    auto result = try_install(move(pending));
    VERIFY(!result.is_error());
    return result.release_value();
}

// Moves a staged package into the extension's package directory, keeping the one it replaces aside until the new one
// loads; any failure puts the old one back. A crash in between leaves the old one as a backup that startup restores.
static ErrorOr<NonnullRefPtr<Extension>, String> move_package_into_place(ByteString const& staged, ByteString const& extension_directory, String const& id, String const& locale)
{
    auto package = LexicalPath::join(extension_directory, "package"sv).string();
    auto backup = LexicalPath::join(extension_directory, ByteString::formatted("{}{}", backup_prefix, AK::generate_random_uuid())).string();
    auto io_error = [](StringView step, Error const& error) { return MUST(String::formatted("Unable to install the extension ({}): {}", step, error)); };
    if (auto created = Core::Directory::create(extension_directory, Core::Directory::CreateDirectories::Yes, 0700); created.is_error())
        return io_error("its directory"sv, created.error());
    bool had_package = FileSystem::exists(package);
    if (had_package) {
        if (auto moved = FileSystem::rename_without_replacing(backup, package); moved.is_error())
            return io_error("setting the old version aside"sv, moved.error());
    }
    auto restore = [&] {
        if (FileSystem::exists(package))
            (void)remove_extension_package(package);
        if (had_package)
            (void)FileSystem::rename_without_replacing(package, backup);
    };
    if (auto moved = FileSystem::rename_without_replacing(package, staged); moved.is_error()) {
        restore();
        return io_error("moving the new version in"sv, moved.error());
    }
    auto loaded = Extension::load_unpacked(package, id, {}, locale);
    if (loaded.is_error()) {
        restore();
        return loaded.release_error();
    }
    if (had_package)
        (void)remove_extension_package(backup);
    return loaded.release_value();
}

ErrorOr<NonnullRefPtr<Extension>, String> ExtensionRegistry::try_install(PendingExtensionInstall pending)
{
    auto extension = move(pending.extension);
    Vector<String> grants;
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id == extension->id()) {
            grants = installed.granted_permissions;
            break;
        }
    }
    for (auto const& permission : pending.newly_declared_permissions) {
        if (!grants.contains_slow(permission))
            grants.append(permission);
    }
    quick_sort(grants);
    // A packed extension's files move from where they were staged into its package directory, replacing any it had;
    // nothing about the installed extension changes unless that works.
    if (pending.staged_package.has_value()) {
        auto moved = move_package_into_place(*pending.staged_package, LexicalPath::join(extensions_directory(), extension->id()).string(), extension->id(), extension_locale(m_settings));
        if (moved.is_error()) {
            if (FileSystem::exists(*pending.staged_package))
                (void)remove_extension_package(*pending.staged_package);
            return moved.release_error();
        }
        extension = moved.release_value();
    }
    extension->apply_permission_grants(grants);
    apply_registered_scripts(*extension);
    if (m_message_broker)
        m_message_broker->close_channels_for(extension->id());
    m_settings.add_installed_extension(extension->directory(), extension->id(), move(grants));
    if (pending.staged_package.has_value())
        m_settings.set_installed_extension_package(extension->id(), extension->manifest().gecko_id);
    m_failed_extensions.remove_all_matching([&](auto const& failed) { return failed.id == extension->id(); });
    bool replaced = false;
    for (auto& loaded : m_extensions) {
        if (loaded->id() == extension->id()) {
            loaded = extension;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        m_extensions.append(extension);
    notify_changed();
    return extension;
}

ErrorOr<NonnullRefPtr<Extension>, String> ExtensionRegistry::load_unpacked(ByteString directory)
{
    auto pending = TRY(prepare_unpacked(move(directory)));
    if (pending.is_reload)
        pending.newly_declared_permissions.clear();
    return install(move(pending));
}

void ExtensionRegistry::record_failure(String id, ByteString directory, String error)
{
    if (m_message_broker)
        m_message_broker->close_channels_for(id);
    m_extensions.remove_all_matching([&](auto const& extension) { return extension->id() == id; });
    m_failed_extensions.remove_all_matching([&](auto const& failed) { return failed.id == id; });
    m_failed_extensions.append({ move(id), move(directory), move(error) });
}

ErrorOr<NonnullRefPtr<Extension>, String> ExtensionRegistry::reload(StringView id)
{
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id != id)
            continue;
        auto result = prepare_unpacked(installed.path);
        if (result.is_error()) {
            auto error = result.release_error();
            record_failure(installed.id, installed.path, error);
            notify_changed();
            return error;
        }
        auto pending = result.release_value();
        pending.newly_declared_permissions.clear();
        return install(move(pending));
    }
    return "extension isn't installed"_string;
}

void ExtensionRegistry::remove(StringView id)
{
    Optional<String> installed_id;
    Optional<ByteString> package;
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id == id) {
            installed_id = installed.id;
            // A packed extension's files are Ladybird's, but an unpacked extension's folder is the user's.
            // Only the package directory Ladybird made for it, under the profile, is deleted.
            auto expected = LexicalPath::join(extensions_directory(), installed.id, "package"sv).string();
            if (installed.packed && installed.path == expected)
                package = installed.path;
            break;
        }
    }
    if (!installed_id.has_value())
        return;
    if (m_message_broker)
        m_message_broker->close_channels_for(*installed_id);
    m_extensions.remove_all_matching([&](auto const& extension) { return extension->id() == *installed_id; });
    m_failed_extensions.remove_all_matching([&](auto const& failed) { return failed.id == *installed_id; });
    m_settings.remove_installed_extension(*installed_id);
    if (m_registered_scripts)
        m_registered_scripts->delete_scripts(*installed_id);
    if (package.has_value()) {
        if (auto removed = remove_extension_package(*package); removed.is_error())
            warnln("Unable to delete extension {} files: {}", *installed_id, removed.error());
    }
    notify_changed();
    for (auto* store : m_storage_stores) {
        store->delete_storage(*installed_id, [id = *installed_id](ErrorOr<void> result) {
            if (result.is_error())
                warnln("Unable to delete extension {} storage: {}", id, result.error());
        });
    }
}

void ExtensionRegistry::set_enabled(StringView id, bool enabled)
{
    auto extension = find(id);
    if (!extension || is_enabled(id) == enabled)
        return;
    if (m_message_broker)
        m_message_broker->close_channels_for(extension->id());
    extension->renew_instance();
    m_settings.set_installed_extension_enabled(id, enabled);
    notify_changed();
}

bool ExtensionRegistry::is_allowed_in_private(StringView id) const
{
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id == id)
            return installed.allowed_in_private;
    }
    return false;
}

void ExtensionRegistry::set_allowed_in_private(StringView id, bool allowed)
{
    if (!find(id) || is_allowed_in_private(id) == allowed)
        return;
    m_settings.set_installed_extension_allowed_in_private(id, allowed);
    notify_changed();
}

void ExtensionRegistry::set_granted(StringView id, Vector<String> api_permissions, Vector<String> origins)
{
    auto extension = find(id);
    if (!extension)
        return;
    auto grants = move(api_permissions);
    grants.extend(move(origins));
    quick_sort(grants);
    m_settings.set_installed_extension_granted_permissions(id, grants);
    extension->apply_permission_grants(grants);
    notify_changed();
}

void ExtensionRegistry::restore_from_settings()
{
    recover_interrupted_installs();
    m_extensions.clear();
    m_failed_extensions.clear();
    for (auto const& installed : m_settings.installed_extensions()) {
        auto extension = Extension::load_unpacked(installed.path, installed.id, installed.granted_permissions, extension_locale(m_settings));
        if (extension.is_error()) {
            record_failure(installed.id, installed.path, extension.release_error());
            continue;
        }
        apply_registered_scripts(*extension.value());
        m_extensions.append(extension.release_value());
    }
    notify_changed();
}

void ExtensionRegistry::apply_registered_scripts(Extension& extension)
{
    if (!m_registered_scripts)
        return;
    Vector<Web::Extensions::ContentScriptDescriptor> scripts;
    for (auto const& script : m_registered_scripts->scripts(extension.id())) {
        // A file that went missing since registration leaves its script out until it's back.
        auto descriptor = extension.registered_content_script(script);
        if (descriptor.is_error()) {
            warnln("Extension {} has a registered content script that won't load: {}", extension.id(), descriptor.error());
            continue;
        }
        scripts.append(descriptor.release_value());
    }
    extension.set_registered_content_scripts(move(scripts));
}

void ExtensionRegistry::registered_scripts_changed(StringView id)
{
    auto extension = find(id);
    if (!extension)
        return;
    apply_registered_scripts(*extension);
    notify_changed();
}

RefPtr<Extension> ExtensionRegistry::find(StringView id) const
{
    for (auto const& extension : m_extensions) {
        if (extension->id() == id)
            return extension;
    }
    return nullptr;
}

bool ExtensionRegistry::is_enabled(StringView id) const
{
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id == id)
            return installed.enabled;
    }
    return false;
}

Vector<Web::Extensions::ExtensionDescriptor> ExtensionRegistry::enabled_descriptors(IsPrivate is_private) const
{
    Vector<Web::Extensions::ExtensionDescriptor> descriptors;
    for (auto const& extension : m_extensions) {
        if (is_enabled(extension->id()) && (is_private == IsPrivate::No || is_allowed_in_private(extension->id())))
            descriptors.append(extension->descriptor());
    }
    return descriptors;
}

bool ExtensionRegistry::may_access_resource(StringView id, StringView path, URL::URL const& document_url, IsPrivate is_private) const
{
    auto extension = find(id);
    if (!extension || !is_enabled(id) || (is_private == IsPrivate::Yes && !is_allowed_in_private(id)) || Web::Extensions::canonical_resource_path(path).is_error())
        return false;
    if (document_url.scheme() == Web::Extensions::scheme && document_url.serialized_host() == id)
        return true;
    return extension->is_web_accessible(path, document_url);
}

bool ExtensionRegistry::may_create_context(StringView id, URL::URL const& document_url, bool is_top_level, bool inherited, IsPrivate is_private) const
{
    auto extension = find(id);
    if (!extension || !is_enabled(id) || (is_private == IsPrivate::Yes && !is_allowed_in_private(id)))
        return false;
    if (!inherited && document_url.scheme() == Web::Extensions::scheme && document_url.serialized_host() == id)
        return true;
    for (auto const& script : extension->descriptor().content_scripts) {
        if ((inherited && !script.match_about_blank) || (!is_top_level && !script.all_frames) || script.world != Web::Extensions::ExecutionWorld::Isolated)
            continue;
        if (script.exclude_matches.contains([&](auto const& pattern) { return pattern.matches(document_url); }))
            continue;
        if (script.id.has_value() && !extension->descriptor().has_host_access(document_url))
            continue;
        if (script.matches.contains([&](auto const& pattern) { return pattern.matches(document_url); }))
            return true;
    }
    return false;
}

void ExtensionRegistry::notify_changed()
{
    auto observers = m_observers;
    for (auto& observer : observers) {
        if (observer)
            observer->extensions_changed();
    }
}

}
