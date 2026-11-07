/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Extensions/ExtensionWorldEnvironmentSettingsObject.h>
#include <LibWeb/HTML/PolicyContainers.h>

namespace Web::Extensions {

GC_DEFINE_ALLOCATOR(ExtensionWorldEnvironmentSettingsObject);

GC::Ref<ExtensionWorldEnvironmentSettingsObject> ExtensionWorldEnvironmentSettingsObject::create(DOM::Document& document, NonnullOwnPtr<JS::ExecutionContext> execution_context)
{
    auto& document_settings = document.relevant_settings_object();
    auto& realm = *execution_context->realm();
    auto settings = realm.create<ExtensionWorldEnvironmentSettingsObject>(document, document_settings, move(execution_context));
    settings->id = HTML::EnvironmentId::generate();
    settings->creation_url = document_settings.creation_url;
    settings->top_level_creation_url = document_settings.top_level_creation_url;
    settings->top_level_origin = document_settings.top_level_origin;
    settings->target_browsing_context = document_settings.target_browsing_context;
    settings->execution_ready = true;
    return settings;
}

ExtensionWorldEnvironmentSettingsObject::ExtensionWorldEnvironmentSettingsObject(DOM::Document& document, HTML::EnvironmentSettingsObject& document_settings, NonnullOwnPtr<JS::ExecutionContext> execution_context)
    : EnvironmentSettingsObject(move(execution_context))
    , m_document(document)
    , m_document_settings(document_settings)
{
}

ExtensionWorldEnvironmentSettingsObject::~ExtensionWorldEnvironmentSettingsObject() = default;

void ExtensionWorldEnvironmentSettingsObject::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_document_settings);
}

GC::Ptr<DOM::Document> ExtensionWorldEnvironmentSettingsObject::responsible_document()
{
    return m_document;
}

URL::URL ExtensionWorldEnvironmentSettingsObject::api_base_url() const
{
    return m_document_settings->api_base_url();
}

URL::Origin ExtensionWorldEnvironmentSettingsObject::origin() const
{
    return m_document_settings->origin();
}

bool ExtensionWorldEnvironmentSettingsObject::has_cross_site_ancestor() const
{
    return m_document_settings->has_cross_site_ancestor();
}

NonnullRefPtr<HTML::PolicyContainer> ExtensionWorldEnvironmentSettingsObject::policy_container() const
{
    return m_document_settings->policy_container();
}

HTML::CanUseCrossOriginIsolatedAPIs ExtensionWorldEnvironmentSettingsObject::cross_origin_isolated_capability() const
{
    return m_document_settings->cross_origin_isolated_capability();
}

Optional<u64> ExtensionWorldEnvironmentSettingsObject::agent_cluster_id() const
{
    return m_document_settings->agent_cluster_id();
}

HTML::CrossOriginIsolationMode ExtensionWorldEnvironmentSettingsObject::agent_cluster_cross_origin_isolation_mode() const
{
    return m_document_settings->agent_cluster_cross_origin_isolation_mode();
}

double ExtensionWorldEnvironmentSettingsObject::time_origin() const
{
    return m_document_settings->time_origin();
}

}
