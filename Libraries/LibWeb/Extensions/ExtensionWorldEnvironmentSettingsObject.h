/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::Extensions {

// The settings object of one extension world: the document's environment, with the world's realm. A script of the
// world is created with it, so the modules the script imports are parsed in the world's realm and kept in a module
// map of the world's own, apart from the page's. Everything else about the environment is the document's.
class ExtensionWorldEnvironmentSettingsObject final : public HTML::EnvironmentSettingsObject {
    GC_CELL(ExtensionWorldEnvironmentSettingsObject, HTML::EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(ExtensionWorldEnvironmentSettingsObject);

public:
    static GC::Ref<ExtensionWorldEnvironmentSettingsObject> create(DOM::Document&, NonnullOwnPtr<JS::ExecutionContext>);
    virtual ~ExtensionWorldEnvironmentSettingsObject() override;

    virtual GC::Ptr<DOM::Document> responsible_document() override;
    virtual URL::URL api_base_url() const override;
    virtual URL::Origin origin() const override;
    virtual bool has_cross_site_ancestor() const override;
    virtual NonnullRefPtr<HTML::PolicyContainer> policy_container() const override;
    virtual HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    virtual Optional<u64> agent_cluster_id() const override;
    virtual HTML::CrossOriginIsolationMode agent_cluster_cross_origin_isolation_mode() const override;
    virtual double time_origin() const override;

private:
    ExtensionWorldEnvironmentSettingsObject(DOM::Document&, HTML::EnvironmentSettingsObject& document_settings, NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    GC::Ref<HTML::EnvironmentSettingsObject> m_document_settings;
};

}
