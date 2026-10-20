/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Settings.h>

namespace WebView {

ExtensionRegistry::ExtensionRegistry(Settings& settings)
    : m_settings(settings)
{
}

ErrorOr<NonnullRefPtr<Extension>, String> ExtensionRegistry::load_unpacked(ByteString directory)
{
    auto canonical_directory = FileSystem::real_path(directory);
    if (canonical_directory.is_error())
        return MUST(String::formatted("Can't resolve extension directory: {}", canonical_directory.error()));
    directory = canonical_directory.release_value();
    Optional<String> existing_id;
    for (auto const& installed : m_settings.installed_extensions()) {
        auto installed_directory = FileSystem::real_path(installed.path);
        if (!installed_directory.is_error() && installed_directory.value() == directory)
            existing_id = installed.id;
    }

    // Load before persisting, so a directory that doesn't hold a valid extension is never installed.
    auto id = existing_id.value_or_lazy_evaluated([&] { return AK::generate_random_uuid(); });
    auto extension = TRY(Extension::load_unpacked(directory, id));

    m_settings.add_installed_extension(directory, id);

    m_extensions.remove_all_matching([&](auto const& loaded) { return loaded->id() == extension->id(); });
    m_extensions.append(extension);
    notify_changed();
    return extension;
}

void ExtensionRegistry::remove(StringView id)
{
    auto removed = m_extensions.remove_all_matching([&](auto const& extension) { return extension->id() == id; });
    m_settings.remove_installed_extension(id);
    if (removed)
        notify_changed();
}

void ExtensionRegistry::set_enabled(StringView id, bool enabled)
{
    auto extension = find(id);
    if (!extension || is_enabled(id) == enabled)
        return;
    extension->renew_instance();
    m_settings.set_installed_extension_enabled(id, enabled);
    notify_changed();
}

void ExtensionRegistry::restore_from_settings()
{
    Vector<String> ids_to_forget;
    for (auto const& installed : m_settings.installed_extensions()) {
        auto extension = Extension::load_unpacked(installed.path, installed.id);
        if (extension.is_error()) {
            dbgln("Not restoring extension {} from {}: {}", installed.id, installed.path, extension.error());
            ids_to_forget.append(installed.id);
            continue;
        }
        m_extensions.append(extension.release_value());
    }
    for (auto const& id : ids_to_forget)
        m_settings.remove_installed_extension(id);
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

Vector<Web::Extensions::ExtensionDescriptor> ExtensionRegistry::enabled_descriptors() const
{
    Vector<Web::Extensions::ExtensionDescriptor> descriptors;
    for (auto const& extension : m_extensions) {
        if (is_enabled(extension->id()))
            descriptors.append(extension->descriptor());
    }
    return descriptors;
}

void ExtensionRegistry::notify_changed()
{
    if (on_changed)
        on_changed();
}

}
