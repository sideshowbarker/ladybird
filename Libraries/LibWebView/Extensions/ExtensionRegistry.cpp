/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Extensions/ResourcePath.h>
#include <LibWebCommon/Extensions/Scheme.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
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

NonnullRefPtr<Extension> ExtensionRegistry::install(PendingExtensionInstall pending)
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
    extension->apply_permission_grants(grants);
    if (m_message_broker)
        m_message_broker->close_channels_for(extension->id());
    m_settings.add_installed_extension(extension->directory(), extension->id(), move(grants));
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
    for (auto const& installed : m_settings.installed_extensions()) {
        if (installed.id == id) {
            installed_id = installed.id;
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
    m_extensions.clear();
    m_failed_extensions.clear();
    for (auto const& installed : m_settings.installed_extensions()) {
        auto extension = Extension::load_unpacked(installed.path, installed.id, installed.granted_permissions, extension_locale(m_settings));
        if (extension.is_error()) {
            record_failure(installed.id, installed.path, extension.release_error());
            continue;
        }
        m_extensions.append(extension.release_value());
    }
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
