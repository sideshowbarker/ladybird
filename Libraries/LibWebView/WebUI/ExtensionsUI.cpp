/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI/ExtensionsUI.h>

namespace WebView {

ExtensionsUI::ExtensionsUI(WebContentClient& client, Web::PageId page_id, NonnullOwnPtr<IPC::Transport> transport, String host)
    : WebUI(client, page_id, move(transport), move(host))
    , ExtensionRegistryObserver(Application::extension_registry())
{
}

static Optional<String> installed_id_from_message(JsonValue const& data)
{
    if (!data.is_object())
        return {};
    auto id = data.as_object().get_string("id"sv);
    if (!id.has_value())
        return {};
    for (auto const& installed : Application::settings().installed_extensions()) {
        if (installed.id == *id)
            return *id;
    }
    return {};
}

void ExtensionsUI::register_interfaces()
{
    register_interface("loadExtensions"sv, [this](auto const&) { send_snapshot(); });
    register_interface("setExtensionEnabled"sv, [](auto const& data) {
        auto id = installed_id_from_message(data);
        if (!id.has_value())
            return;
        if (auto enabled = data.as_object().get_bool("enabled"sv); enabled.has_value())
            Application::extension_registry().set_enabled(*id, *enabled);
    });
    register_interface("setExtensionAllowedInPrivate"sv, [](auto const& data) {
        auto id = installed_id_from_message(data);
        if (!id.has_value())
            return;
        if (auto allowed = data.as_object().get_bool("allowed"sv); allowed.has_value())
            Application::extension_registry().set_allowed_in_private(*id, *allowed);
    });
    register_interface("reloadExtension"sv, [this](auto const& data) {
        auto id = installed_id_from_message(data);
        if (!id.has_value())
            return;
        auto result = Application::extension_registry().reload(*id);
        send_result("reload"sv, result.is_error() ? result.release_error() : String {});
    });
    register_interface("removeExtension"sv, [this](auto const& data) {
        if (!consume_user_activation())
            return;
        if (auto id = installed_id_from_message(data); id.has_value())
            Application::extension_registry().remove(*id);
    });
    register_interface("openExtensionOptions"sv, [this](auto const& data) {
        if (!consume_user_activation())
            return;
        auto id = installed_id_from_message(data);
        if (!id.has_value())
            return;
        if (auto extension = Application::extension_registry().find(*id))
            (void)Application::the().open_extension_options_page(*extension, {});
    });
    register_interface("loadUnpackedExtension"sv, [this](auto const&) { load(Source::Directory); });
    register_interface("installExtensionFromFile"sv, [this](auto const&) { load(Source::Archive); });
    register_interface("installExtension"sv, [this](auto const& data) { install(data); });
    register_interface("cancelExtensionInstall"sv, [this](auto const& data) {
        if (!data.is_object())
            return;
        if (auto token = data.as_object().get_string("token"sv); token.has_value())
            m_install_requests.cancel(*token);
    });
}

bool ExtensionsUI::is_active() const
{
    if (!has_client() || !is_open())
        return false;
    auto* page = client().page(page_id());
    return page && page->web_ui().ptr() == this;
}

bool ExtensionsUI::consume_user_activation()
{
    if (!is_active())
        return false;
    auto* page = client().page(page_id());
    return page && page->consume_transient_user_activation();
}

void ExtensionsUI::extensions_changed()
{
    ++m_picker_generation;
    m_install_requests.clear();
    send_snapshot();
}

static String display_path(StringView path)
{
    return String::from_utf8_with_replacement_character(path, String::WithBOMHandling::No);
}

void ExtensionsUI::send_snapshot()
{
    if (!is_active())
        return;

    auto& registry = Application::extension_registry();
    HashMap<String, Extension const*> loaded;
    HashMap<String, FailedExtension const*> failed;
    for (auto const& extension : registry.extensions())
        loaded.set(extension->id(), extension.ptr());
    for (auto const& extension : registry.failed_extensions())
        failed.set(extension.id, &extension);

    JsonArray snapshot;
    for (auto const& installed : Application::settings().installed_extensions()) {
        JsonObject entry;
        entry.set("id"sv, installed.id);
        entry.set("directory"sv, display_path(installed.path));
        entry.set("enabled"sv, installed.enabled);
        entry.set("allowedInPrivate"sv, installed.allowed_in_private);
        JsonArray permissions;
        for (auto const& permission : installed.granted_permissions)
            permissions.must_append(permission);
        if (auto extension = loaded.get(installed.id); extension.has_value()) {
            auto const& manifest = (*extension)->manifest();
            entry.set("name"sv, manifest.name);
            entry.set("version"sv, manifest.version);
            entry.set("description"sv, manifest.description);
            entry.set("hasOptions"sv, manifest.options_page.has_value());
            entry.set("error"sv, String {});
            for (auto const& pattern : (*extension)->descriptor().host_permissions)
                permissions.must_append(pattern.to_string());
        } else {
            entry.set("name"sv, display_path(LexicalPath(installed.path).basename()));
            entry.set("version"sv, String {});
            entry.set("description"sv, String {});
            auto failure = failed.get(installed.id);
            entry.set("error"sv, failure.has_value() ? (*failure)->error : "extension isn't loaded"_string);
        }
        entry.set("permissions"sv, move(permissions));
        snapshot.must_append(move(entry));
    }
    async_send_message("extensionsSnapshot"sv, move(snapshot));
}

void ExtensionsUI::send_result(StringView operation, String error)
{
    if (!is_active())
        return;
    JsonObject result;
    result.set("operation"sv, operation);
    result.set("error"sv, move(error));
    async_send_message("extensionOperationResult"sv, move(result));
}

void ExtensionsUI::load(Source source)
{
    if (!consume_user_activation())
        return;
    m_install_requests.clear();
    auto generation = ++m_picker_generation;
    auto chosen = [weak_this = WebUI::make_weak_ptr(), generation, source](Optional<ByteString> path) {
        if (!weak_this)
            return;
        auto& ui = static_cast<ExtensionsUI&>(*weak_this);
        if (!ui.is_active() || generation != ui.m_picker_generation)
            return;
        if (!path.has_value()) {
            ui.send_result("load"sv);
            return;
        }
        auto& registry = Application::extension_registry();
        auto result = source == Source::Archive ? registry.prepare_packed(path.release_value()) : registry.prepare_unpacked(path.release_value());
        if (result.is_error()) {
            ui.send_result("load"sv, result.release_error());
            return;
        }
        auto pending = result.release_value();
        if (!install_needs_confirmation(pending)) {
            auto installed = Application::extension_registry().try_install(move(pending));
            ui.send_result("load"sv, installed.is_error() ? installed.release_error() : String {});
            return;
        }
        JsonObject confirmation;
        confirmation.set("name"sv, pending.extension->manifest().name);
        JsonArray permissions;
        for (auto const& permission : pending.newly_declared_permissions)
            permissions.must_append(permission);
        confirmation.set("permissions"sv, move(permissions));
        JsonArray hosts;
        for (auto const& pattern : pending.extension->descriptor().host_permissions)
            hosts.must_append(pattern.to_string());
        confirmation.set("hostPermissions"sv, move(hosts));
        confirmation.set("replaces"sv, pending.replaces.value_or({}));
        confirmation.set("token"sv, ui.m_install_requests.begin(move(pending)));
        ui.async_send_message("confirmExtensionInstall"sv, move(confirmation));
    };
    if (source == Source::Archive)
        Application::the().ask_user_for_extension_file(move(chosen));
    else
        Application::the().ask_user_for_extension_directory(move(chosen));
}

void ExtensionsUI::install(JsonValue const& data)
{
    if (!consume_user_activation() || !data.is_object())
        return;
    auto token = data.as_object().get_string("token"sv);
    if (!token.has_value())
        return;
    auto pending = m_install_requests.take(*token);
    if (!pending.has_value())
        return;
    auto installed = Application::extension_registry().try_install(pending.release_value());
    send_result("load"sv, installed.is_error() ? installed.release_error() : String {});
}

}
