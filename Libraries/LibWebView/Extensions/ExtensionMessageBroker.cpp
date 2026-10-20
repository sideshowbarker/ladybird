/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>

namespace WebView {

void ExtensionMessageBroker::context_created(Endpoint& endpoint, String const& extension_id, u64 context_id, Web::HTML::CrossProcessId document_id, u64 revision)
{
    if (endpoint.extension_revision(extension_id) != revision || !endpoint.extension_message_sender(extension_id, document_id).has_value())
        return;
    auto& contexts = m_contexts.ensure(&endpoint);
    if (!contexts.contains(context_id))
        contexts.set(context_id, Context { extension_id, document_id, revision });
}

void ExtensionMessageBroker::context_destroyed(Endpoint& endpoint, String const& extension_id, u64 context_id)
{
    auto contexts = m_contexts.get(&endpoint);
    if (!contexts.has_value())
        return;
    auto context = contexts->get(context_id);
    if (!context.has_value() || context->extension_id != extension_id)
        return;
    contexts->remove(context_id);
    m_queued_messages.remove_all_matching([&](auto const& message) {
        return message.source == &endpoint && message.context_id == context_id;
    });

    Vector<u64> pending_ids;
    for (auto const& [id, pending] : m_pending_requests) {
        if (pending.source == &endpoint && pending.source_context_id == context_id)
            pending_ids.append(id);
    }
    for (auto id : pending_ids)
        drop_request(id);

    Vector<u64> dispatch_ids;
    for (auto const& [id, target] : m_dispatch_to_endpoint) {
        if (target.endpoint == &endpoint && target.context_id == context_id)
            dispatch_ids.append(id);
    }
    for (auto id : dispatch_ids)
        reply(endpoint, id, {});
}

void ExtensionMessageBroker::reconcile_contexts()
{
    struct RemovedContext {
        Endpoint* endpoint;
        String extension_id;
        u64 context_id;
    };
    Vector<RemovedContext> removed;
    for (auto const& [endpoint, contexts] : m_contexts) {
        for (auto const& [id, context] : contexts) {
            if (endpoint->extension_revision(context.extension_id) != context.revision)
                removed.append({ endpoint, context.extension_id, id });
        }
    }
    for (auto const& context : removed)
        context_destroyed(*context.endpoint, context.extension_id, context.context_id);
}

void ExtensionMessageBroker::endpoint_gone(Endpoint& endpoint)
{
    auto contexts = m_contexts.get(&endpoint);
    if (!contexts.has_value())
        return;
    auto copy = *contexts;
    for (auto const& [id, context] : copy)
        context_destroyed(endpoint, context.extension_id, id);
    m_contexts.remove(&endpoint);
}

void ExtensionMessageBroker::background_starting(String const& extension_id, u64 revision)
{
    Optional<u64> previous = m_starting_backgrounds.get(extension_id);
    if (previous.has_value() && *previous != revision)
        background_finished(extension_id, *previous, false);
    m_starting_backgrounds.set(extension_id, revision);
}

void ExtensionMessageBroker::background_finished(String const& extension_id, u64 revision, bool success)
{
    if (m_starting_backgrounds.get(extension_id) != revision)
        return;
    m_starting_backgrounds.remove(extension_id);
    Vector<QueuedMessage> messages;
    m_queued_messages.remove_all_matching([&](auto const& message) {
        if (message.extension_id != extension_id || message.revision != revision)
            return false;
        messages.append(message);
        return true;
    });
    for (auto const& message : messages) {
        // An earlier completion can destroy another queued sender.
        auto contexts = m_contexts.get(message.source);
        if (!contexts.has_value() || !contexts->contains(message.context_id))
            continue;
        if (success)
            send(*message.source, message.extension_id, message.request_id, {}, message.json, message.context_id);
        else
            message.source->complete_extension_runtime_message(message.request_id, {});
    }
}

void ExtensionMessageBroker::send(Endpoint& source, String const& extension_id, u64 request_id, Web::Extensions::MessageSender const&, String const& message_json, u64 context_id)
{
    auto registered = m_contexts.get(&source);
    auto context = registered.has_value() ? registered->get(context_id) : Optional<Context&> {};
    auto sender = context.has_value() && context->extension_id == extension_id && source.extension_revision(extension_id) == context->revision
        ? source.extension_message_sender(extension_id, context->document_id)
        : Optional<Web::Extensions::MessageSender> {};
    if (!sender.has_value()) {
        source.complete_extension_runtime_message(request_id, {});
        return;
    }
    if (m_starting_backgrounds.get(extension_id) == context->revision) {
        m_queued_messages.append({ &source, extension_id, request_id, context_id, context->revision, message_json });
        return;
    }
    Vector<Target> targets;
    for (auto& [endpoint, contexts] : m_contexts) {
        for (auto const& [id, target] : contexts) {
            if ((endpoint == &source && id == context_id) || target.extension_id != extension_id || endpoint->extension_revision(extension_id) != target.revision)
                continue;
            auto target_sender = endpoint->extension_message_sender(extension_id, target.document_id);
            if (!target_sender.has_value() || !target_sender->url.has_value())
                continue;
            auto target_url = URL::Parser::basic_parse(*target_sender->url);
            if (target_url.has_value() && target_url->scheme() == "ladybird-extension"sv && target_url->serialized_host() == extension_id)
                targets.append({ endpoint, id });
        }
    }
    if (targets.is_empty()) {
        source.complete_extension_runtime_message(request_id, {});
        return;
    }

    auto pending_id = m_next_pending_id++;
    m_pending_requests.set(pending_id, PendingRequest { .source = &source, .request_id = request_id, .source_context_id = context_id, .outstanding = targets.size() });
    Vector<u64> dispatch_ids;
    for (auto target : targets) {
        auto dispatch_id = m_next_dispatch_id++;
        m_dispatch_to_pending.set(dispatch_id, pending_id);
        m_dispatch_to_endpoint.set(dispatch_id, target);
        dispatch_ids.append(dispatch_id);
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        if (m_dispatch_to_pending.contains(dispatch_ids[i]))
            targets[i].endpoint->dispatch_extension_runtime_message(extension_id, dispatch_ids[i], *sender, message_json, targets[i].context_id);
    }
}

void ExtensionMessageBroker::reply(Endpoint& target, u64 dispatch_id, Optional<String> const& reply_json)
{
    Optional<u64> pending_id = m_dispatch_to_pending.get(dispatch_id);
    auto endpoint = m_dispatch_to_endpoint.get(dispatch_id);
    if (!pending_id.has_value() || !endpoint.has_value() || endpoint->endpoint != &target)
        return;
    m_dispatch_to_pending.remove(dispatch_id);
    m_dispatch_to_endpoint.remove(dispatch_id);

    if (reply_json.has_value())
        settle(*pending_id, reply_json);
    else
        decline(*pending_id);
}

void ExtensionMessageBroker::drop_request(u64 pending_id)
{
    m_pending_requests.remove(pending_id);
    Vector<u64> dispatches;
    for (auto const& [id, pending] : m_dispatch_to_pending) {
        if (pending == pending_id)
            dispatches.append(id);
    }
    for (auto id : dispatches) {
        m_dispatch_to_pending.remove(id);
        m_dispatch_to_endpoint.remove(id);
    }
}

void ExtensionMessageBroker::settle(u64 pending_id, Optional<String> const& reply_json)
{
    auto pending = m_pending_requests.get(pending_id);
    if (!pending.has_value())
        return;
    auto* source = pending->source;
    auto request_id = pending->request_id;
    drop_request(pending_id);
    source->complete_extension_runtime_message(request_id, reply_json);
}

void ExtensionMessageBroker::decline(u64 pending_id)
{
    auto pending = m_pending_requests.get(pending_id);
    if (!pending.has_value())
        return;
    if (--pending->outstanding == 0)
        settle(pending_id, {});
}

}
