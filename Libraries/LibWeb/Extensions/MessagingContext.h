/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Extensions/ExtensionDescriptor.h>
#include <LibWebCommon/Extensions/MessageSender.h>

namespace Web::Extensions {

// A JavaScript context of one extension that can send and receive runtime messages: a content script's world in a
// document, or one of the extension's own documents. The router keeps the live ones, by extension id.
class WEB_API MessagingContext : public GC::Cell {
    GC_CELL(MessagingContext, GC::Cell);

public:
    virtual ~MessagingContext() override = default;

    u64 context_id() const { return m_context_id; }
    virtual DOM::Document& document() = 0;

    virtual JS::Realm& realm() = 0;
    virtual ExtensionDescriptor const& descriptor() const = 0;
    virtual EventListenerList& message_listeners() = 0;
    virtual Page& page() = 0;
    virtual MessageSender sender() const = 0;
    virtual bool is_detached() const = 0;
    virtual void detach() = 0;

    String const& extension_id() const { return descriptor().id; }

protected:
    MessagingContext()
        : m_context_id(++s_next_context_id)
    {
    }

private:
    inline static u64 s_next_context_id { 0 };
    u64 m_context_id;
};

}
