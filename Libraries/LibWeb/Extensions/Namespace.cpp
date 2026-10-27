/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Extensions/API/Action.h>
#include <LibWeb/Extensions/API/Alarms.h>
#include <LibWeb/Extensions/API/Commands.h>
#include <LibWeb/Extensions/API/ContextMenus.h>
#include <LibWeb/Extensions/API/I18n.h>
#include <LibWeb/Extensions/API/Permissions.h>
#include <LibWeb/Extensions/API/Runtime.h>
#include <LibWeb/Extensions/API/Scripting.h>
#include <LibWeb/Extensions/API/Storage.h>
#include <LibWeb/Extensions/API/Tabs.h>
#include <LibWeb/Extensions/API/Windows.h>
#include <LibWeb/Extensions/MessagingContext.h>
#include <LibWeb/Extensions/Namespace.h>

namespace Web::Extensions {

GC::Ref<JS::Object> install_namespace(JS::Realm& realm, MessagingContext& messaging_context, NamespaceContext context)
{
    auto attributes = JS::Attribute::Writable | JS::Attribute::Configurable;

    auto namespace_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    namespace_object->define_direct_property("runtime"_utf16_fly_string, create_runtime(realm, messaging_context, context), attributes);

    namespace_object->define_direct_property("i18n"_utf16_fly_string, create_i18n(realm, messaging_context), attributes);

    if (messaging_context.descriptor().has_permission("storage"sv))
        namespace_object->define_direct_property("storage"_utf16_fly_string, create_storage(realm, messaging_context), attributes);

    // Content scripts can't see or change what the extension may do.
    if (context == NamespaceContext::ExtensionDocument)
        namespace_object->define_direct_property("permissions"_utf16_fly_string, create_permissions(realm, messaging_context), attributes);

    // Only manifest version 3 has scripting; version 2 injects with tabs.executeScript().
    if (context == NamespaceContext::ExtensionDocument && messaging_context.descriptor().manifest_version >= 3 && messaging_context.descriptor().has_permission("scripting"sv))
        namespace_object->define_direct_property("scripting"_utf16_fly_string, create_scripting(realm, messaging_context), attributes);

    // Alarms wake the extension's own pages, so content scripts don't have them.
    if (context == NamespaceContext::ExtensionDocument && messaging_context.descriptor().has_permission("alarms"sv))
        namespace_object->define_direct_property("alarms"_utf16_fly_string, create_alarms(realm, messaging_context), attributes);

    // Content scripts act on the page they're in, so only the extension's own documents get tabs.
    if (context == NamespaceContext::ExtensionDocument)
        namespace_object->define_direct_property("tabs"_utf16_fly_string, create_tabs(realm, messaging_context), attributes);
    if (context == NamespaceContext::ExtensionDocument)
        namespace_object->define_direct_property("windows"_utf16_fly_string, create_windows(realm, messaging_context), attributes);
    if (context == NamespaceContext::ExtensionDocument)
        namespace_object->define_direct_property("commands"_utf16_fly_string, create_commands(realm, messaging_context), attributes);
    // Firefox calls the permission and the namespace "menus"; either permission gives both names, for one object.
    if (context == NamespaceContext::ExtensionDocument && (messaging_context.descriptor().has_permission("contextMenus"sv) || messaging_context.descriptor().has_permission("menus"sv))) {
        auto menus = create_context_menus(realm, messaging_context);
        namespace_object->define_direct_property("contextMenus"_utf16_fly_string, menus, attributes);
        namespace_object->define_direct_property("menus"_utf16_fly_string, menus, attributes);
    }

    // The toolbar button belongs to the extension's own documents too, and exists only when the manifest declares one.
    if (context == NamespaceContext::ExtensionDocument && messaging_context.descriptor().has_action) {
        auto name = messaging_context.descriptor().manifest_version == 2 ? "browserAction"_utf16_fly_string : "action"_utf16_fly_string;
        namespace_object->define_direct_property(name, create_action(realm, messaging_context), attributes);
    }

    // Both names refer to the same object, so an extension written against either works.
    auto& global = realm.global_object();
    global.define_direct_property("browser"_utf16_fly_string, namespace_object, attributes);
    global.define_direct_property("chrome"_utf16_fly_string, namespace_object, attributes);
    return namespace_object;
}

}
