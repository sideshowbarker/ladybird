/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>

namespace Web::Extensions {

// The listeners of one extension API event, such as runtime.onMessage, in registration order.
class WEB_API EventListenerList final : public GC::Cell {
    GC_CELL(EventListenerList, GC::Cell);
    GC_DECLARE_ALLOCATOR(EventListenerList);

public:
    void add(JS::FunctionObject&);
    void remove(JS::FunctionObject const&);
    bool has(JS::FunctionObject const&) const;
    bool is_empty() const { return m_listeners.is_empty(); }
    void clear() { m_listeners.clear(); }
    void revoke()
    {
        m_active = false;
        clear();
    }
    bool is_active() const { return m_active; }
    // Called when the list gains its first listener or loses its last through add() or remove().
    void set_on_emptiness_change(GC::Ptr<GC::Function<void()>> callback) { m_on_emptiness_change = callback; }

    // A rooted copy keeps removed listeners alive across callbacks and collection.
    GC::RootVector<GC::Ref<JS::FunctionObject>> listeners() const;

private:
    virtual void visit_edges(Visitor&) override;

    Vector<GC::Ref<JS::FunctionObject>> m_listeners;
    bool m_active { true };
    GC::Ptr<GC::Function<void()>> m_on_emptiness_change;
};

// An extension API event object: { addListener(f), removeListener(f), hasListener(f) } over the given list.
WEB_API GC::Ref<JS::Object> create_event(JS::Realm&, EventListenerList&);

}
