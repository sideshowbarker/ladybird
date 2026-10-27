/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Extensions/WebRequest.h>

namespace Web::Extensions {

class MessagingContext;

// The webRequest listeners of one extension context. Each one the UI process should hear about is reported with its
// filter; a listener whose filter names no valid URL pattern stays registered but is never reported, so it never runs.
class WEB_API WebRequestListeners final : public GC::Cell {
    GC_CELL(WebRequestListeners, GC::Cell);
    GC_DECLARE_ALLOCATOR(WebRequestListeners);

public:
    void add(JS::FunctionObject&, WebRequestListener, bool reported);
    void remove(WebRequestEvent, JS::FunctionObject const&);
    bool has(WebRequestEvent, JS::FunctionObject const&) const;
    GC::Ptr<JS::FunctionObject> callback_for(u64 listener_id) const;
    WebRequestListener const* record_for(u64 listener_id) const;
    void revoke();
    bool is_active() const { return m_active; }

private:
    explicit WebRequestListeners(MessagingContext&);

    virtual void visit_edges(Visitor&) override;
    void report();

    struct Entry {
        WebRequestListener record;
        GC::Ref<JS::FunctionObject> callback;
        bool reported { false };
    };

    GC::Ref<MessagingContext> m_context;
    Vector<Entry> m_entries;
    bool m_active { true };
};

GC::Ref<JS::Object> create_web_request(JS::Realm&, MessagingContext&);

}
