/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/WeakPtr.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/ExtensionMessageBroker.h>
#include <LibWebView/Extensions/ExtensionStorageStore.h>

namespace WebView {

using Web::Extensions::MessageResult;

using Web::Extensions::StorageRequest;
using Web::Extensions::StorageResult;

bool ExtensionMessageBroker::storage_context_is_current(Endpoint* endpoint, u64 context_id, Context const& expected) const
{
    auto registered = m_contexts.get(endpoint);
    auto context = registered.has_value() ? registered->get(context_id) : Optional<Context const&> {};
    return context.has_value() && context->extension_id == expected.extension_id && context->revision == expected.revision
        && context->document_id == expected.document_id && endpoint->extension_revision(expected.extension_id) == expected.revision
        && endpoint->extension_message_sender(expected.extension_id, expected.document_id).has_value();
}

void ExtensionMessageBroker::broadcast_storage_changes(String const& extension_id, u64 revision, String const& changes)
{
    Vector<Target> targets;
    for (auto const& [endpoint, contexts] : m_contexts) {
        for (auto const& [id, context] : contexts) {
            if (context.extension_id == extension_id && context.revision == revision && storage_context_is_current(endpoint, id, context)) {
                targets.append({ endpoint, id });
                break;
            }
        }
    }
    for (auto const& target : targets) {
        auto registered = m_contexts.get(target.endpoint);
        auto context = registered.has_value() ? registered->get(target.context_id) : Optional<Context&> {};
        if (context.has_value() && context->extension_id == extension_id && context->revision == revision
            && storage_context_is_current(target.endpoint, target.context_id, *context)
            && target.endpoint->extension_has_permission(extension_id, "storage"sv))
            target.endpoint->dispatch_extension_storage_changed(extension_id, changes);
    }
}

Optional<ExtensionMessageBroker::CurrentContext> ExtensionMessageBroker::current_context(Endpoint& endpoint, u64 context_id) const
{
    auto registered = m_contexts.get(&endpoint);
    auto context = registered.has_value() ? registered->get(context_id) : Optional<Context const&> {};
    if (!context.has_value() || !storage_context_is_current(&endpoint, context_id, *context))
        return {};
    auto sender = endpoint.extension_message_sender(context->extension_id, context->document_id);
    auto url = sender.has_value() && sender->url.has_value() ? URL::Parser::basic_parse(*sender->url) : Optional<URL::URL> {};
    auto is_extension_document = url.has_value() && url->scheme() == "ladybird-extension"sv && url->serialized_host() == context->extension_id;
    return CurrentContext { context->extension_id, context->document_id, is_extension_document };
}

void ExtensionMessageBroker::request_storage(Endpoint& source, u64 context_id, u64 request_id, StorageRequest const& request, ExtensionStorageStore& store)
{
    auto registered = m_contexts.get(&source);
    auto context = registered.has_value() ? registered->get(context_id) : Optional<Context&> {};
    if (!context.has_value() || !storage_context_is_current(&source, context_id, *context))
        return;
    auto identity = *context;
    auto const& id = identity.extension_id;
    if (!source.extension_has_permission(id, "storage"sv)) {
        source.complete_extension_storage(request_id, { StorageResult::Type::Error, "Missing the storage permission"_string });
        return;
    }
    auto complete = [weak = make_weak_ptr(), endpoint = &source, context_id, request_id, identity](StorageResult result) {
        if (weak && weak->storage_context_is_current(endpoint, context_id, identity))
            endpoint->complete_extension_storage(request_id, result);
    };
    auto changed = [weak = make_weak_ptr(), id, revision = identity.revision, complete](ErrorOr<StorageChanges> result) {
        if (result.is_error()) {
            complete({ StorageResult::Type::Error, MUST(String::formatted("{}", result.error())) });
            return;
        }
        if (weak && !result.value().is_empty()) {
            JsonObject changes;
            for (auto const& [key, change] : result.value()) {
                JsonObject values;
                if (change.old_value.has_value())
                    values.set("oldValue"sv, *change.old_value);
                if (change.new_value.has_value())
                    values.set("newValue"sv, *change.new_value);
                changes.set(key, move(values));
            }
            weak->broadcast_storage_changes(id, revision, changes.serialized());
        }
        complete({});
    };
    switch (request.operation) {
    case StorageRequest::Operation::Get:
        store.get(id, request.keys, [complete](ErrorOr<JsonObject> result) {
            if (result.is_error())
                complete({ StorageResult::Type::Error, MUST(String::formatted("{}", result.error())) });
            else
                complete({ StorageResult::Type::Items, result.value().serialized() });
        });
        break;
    case StorageRequest::Operation::BytesInUse:
        store.bytes_in_use(id, request.keys, [complete](ErrorOr<u64> result) {
            if (result.is_error())
                complete({ StorageResult::Type::Error, MUST(String::formatted("{}", result.error())) });
            else
                complete({ StorageResult::Type::Bytes, {}, result.value() });
        });
        break;
    case StorageRequest::Operation::Set: {
        auto parsed = JsonValue::from_string(request.items_json);
        if (parsed.is_error() || !parsed.value().is_object()) {
            complete({ StorageResult::Type::Error, "Storage items must be a JSON object"_string });
            break;
        }
        store.set(id, parsed.release_value().as_object(), source.extension_has_permission(id, "unlimitedStorage"sv), move(changed));
        break;
    }
    case StorageRequest::Operation::Remove:
        store.remove(id, request.keys.value_or(Vector<String> {}), move(changed));
        break;
    case StorageRequest::Operation::Clear:
        store.clear(id, move(changed));
        break;
    }
}

void ExtensionMessageBroker::context_created(Endpoint& endpoint, String const& extension_id, u64 context_id, Web::HTML::CrossProcessId document_id, u64 revision)
{
    if (endpoint.extension_revision(extension_id) != revision || !endpoint.extension_message_sender(extension_id, document_id).has_value())
        return;
    auto& contexts = m_contexts.ensure(&endpoint);
    if (!contexts.contains(context_id))
        contexts.set(context_id, Context { extension_id, document_id, revision, {} });
}

void ExtensionMessageBroker::context_destroyed(Endpoint& endpoint, String const& extension_id, u64 context_id)
{
    auto contexts = m_contexts.get(&endpoint);
    if (!contexts.has_value())
        return;
    auto context = contexts->get(context_id);
    if (!context.has_value() || context->extension_id != extension_id)
        return;
    auto ports = context->ports;
    contexts->remove(context_id);
    for (auto const& [channel_id, port_id] : ports)
        remove_port(channel_id, { &endpoint, context_id, port_id });
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
    Vector<u64> channels;
    for (auto const& [id, channel] : m_channels) {
        if (channel.extension_id == extension_id && channel.revision == revision && channel.waiting_for_background)
            channels.append(id);
    }
    for (auto id : channels) {
        if (success)
            dispatch_connect(id);
        else
            close_channel(id, "Could not establish connection. Receiving end does not exist."_string);
    }
    for (auto const& message : messages) {
        // An earlier completion can destroy another queued sender.
        auto contexts = m_contexts.get(message.source);
        if (!contexts.has_value() || !contexts->contains(message.context_id))
            continue;
        if (success)
            send(*message.source, message.extension_id, message.request_id, {}, message.json, message.context_id);
        else
            message.source->complete_extension_runtime_message(message.request_id, MessageResult::no_receiver());
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
        source.complete_extension_runtime_message(request_id, MessageResult::no_receiver());
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
        source.complete_extension_runtime_message(request_id, MessageResult::no_receiver());
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

void ExtensionMessageBroker::reply(Endpoint& target, u64 dispatch_id, Web::Extensions::MessageResult const& reply_json)
{
    Optional<u64> pending_id = m_dispatch_to_pending.get(dispatch_id);
    auto endpoint = m_dispatch_to_endpoint.get(dispatch_id);
    if (!pending_id.has_value() || !endpoint.has_value() || endpoint->endpoint != &target)
        return;
    m_dispatch_to_pending.remove(dispatch_id);
    m_dispatch_to_endpoint.remove(dispatch_id);

    if (reply_json.value.has<MessageResult::Reply>() || reply_json.value.has<MessageResult::Error>()) {
        settle(*pending_id, reply_json);
    } else {
        if (reply_json.value.has<MessageResult::NoReply>()) {
            if (auto pending = m_pending_requests.get(*pending_id); pending.has_value())
                pending->had_listener = true;
        }
        decline(*pending_id);
    }
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

void ExtensionMessageBroker::settle(u64 pending_id, Web::Extensions::MessageResult const& reply_json)
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
        settle(pending_id, pending->had_listener ? MessageResult { MessageResult::NoReply {} } : MessageResult::no_receiver());
}

bool ExtensionMessageBroker::owns_port(Endpoint& endpoint, u64 context_id, u64 channel_id, u64 port_id) const
{
    auto contexts = m_contexts.get(&endpoint);
    auto context = contexts.has_value() ? contexts->get(context_id) : Optional<Context const&> {};
    return context.has_value() && context->ports.get(channel_id) == port_id
        && storage_context_is_current(&endpoint, context_id, *context);
}

void ExtensionMessageBroker::connect(Endpoint& source, u64 context_id, u64 port_id, Optional<String> target_extension_id, String name)
{
    auto contexts = m_contexts.get(&source);
    auto context = contexts.has_value() ? contexts->get(context_id) : Optional<Context&> {};
    if (!context.has_value() || !storage_context_is_current(&source, context_id, *context))
        return;
    if (target_extension_id.has_value() && *target_extension_id != context->extension_id) {
        source.complete_extension_runtime_connect(context_id, port_id, 0, "Could not establish connection. Receiving end does not exist."_string);
        return;
    }
    for (auto const& [id, registered_port] : context->ports) {
        if (registered_port == port_id)
            return;
    }
    auto sender = source.extension_message_sender(context->extension_id, context->document_id).release_value();
    auto channel_id = m_next_channel_id++;
    bool waiting = m_starting_backgrounds.get(context->extension_id) == context->revision;
    m_channels.set(channel_id, Channel { .extension_id = context->extension_id, .revision = context->revision, .name = move(name), .sender = sender, .opener = { &source, context_id, port_id, sender.from_background }, .receivers = {}, .queued_messages = {}, .waiting_for_background = waiting });
    context->ports.set(channel_id, port_id);
    source.complete_extension_runtime_connect(context_id, port_id, channel_id, {});
    if (!waiting)
        dispatch_connect(channel_id);
}

void ExtensionMessageBroker::dispatch_connect(u64 channel_id)
{
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value())
        return;
    channel->waiting_for_background = false;
    Vector<u64> ports;
    for (auto& [endpoint, contexts] : m_contexts) {
        for (auto& [context_id, context] : contexts) {
            if ((endpoint == channel->opener.endpoint && context_id == channel->opener.context_id)
                || context.extension_id != channel->extension_id || context.revision != channel->revision
                || !storage_context_is_current(endpoint, context_id, context))
                continue;
            auto sender = endpoint->extension_message_sender(context.extension_id, context.document_id);
            if (!sender.has_value() || !sender->url.has_value())
                continue;
            auto url = URL::Parser::basic_parse(*sender->url);
            if (!url.has_value() || url->scheme() != "ladybird-extension"sv || url->serialized_host() != context.extension_id)
                continue;
            auto port_id = m_next_receiver_port_id++;
            channel->receivers.set(port_id, { endpoint, context_id, port_id, sender->from_background });
            context.ports.set(channel_id, port_id);
            ports.append(port_id);
        }
    }
    channel->outstanding = ports.size();
    auto name = channel->name;
    auto sender = channel->sender;
    for (auto port_id : ports) {
        channel = m_channels.get(channel_id);
        if (!channel.has_value())
            return;
        auto receiver = channel->receivers.get(port_id);
        if (!receiver.has_value())
            continue;
        receiver->dispatched = true;
        auto target = *receiver;
        target.endpoint->dispatch_extension_runtime_connect(target.context_id, channel_id, port_id, name, sender);
    }
    finish_opening(channel_id);
}

void ExtensionMessageBroker::connect_ack(Endpoint& endpoint, u64 context_id, u64 channel_id, bool accepted)
{
    auto contexts = m_contexts.get(&endpoint);
    auto context = contexts.has_value() ? contexts->get(context_id) : Optional<Context&> {};
    auto port_id = context.has_value() ? Optional<u64> { context->ports.get(channel_id) } : Optional<u64> {};
    auto channel = m_channels.get(channel_id);
    if (!port_id.has_value() || !channel.has_value() || !owns_port(endpoint, context_id, channel_id, *port_id))
        return;
    auto receiver = channel->receivers.get(*port_id);
    if (!receiver.has_value() || !receiver->dispatched || receiver->acknowledged)
        return;
    receiver->acknowledged = true;
    --channel->outstanding;
    if (accepted) {
        channel->had_receiver = true;
    } else {
        auto target = *receiver;
        channel->receivers.remove(*port_id);
        forget_port(channel_id, target);
    }
    finish_opening(channel_id);
}

void ExtensionMessageBroker::finish_opening(u64 channel_id)
{
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value() || !channel->opening || channel->waiting_for_background || channel->outstanding)
        return;
    if (channel->receivers.is_empty()) {
        close_channel(channel_id, channel->had_receiver ? Optional<String> {} : "Could not establish connection. Receiving end does not exist."_string);
        return;
    }
    channel->opening = false;
    auto queued = move(channel->queued_messages);
    auto opener = channel->opener;
    for (auto& json : queued)
        port_post_message(*opener.endpoint, opener.context_id, channel_id, opener.port_id, move(json));
}

void ExtensionMessageBroker::port_post_message(Endpoint& endpoint, u64 context_id, u64 channel_id, u64 port_id, String json)
{
    if (!owns_port(endpoint, context_id, channel_id, port_id))
        return;
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value())
        return;
    auto opener = channel->opener;
    if (opener.endpoint != &endpoint || opener.context_id != context_id || opener.port_id != port_id) {
        opener.endpoint->dispatch_extension_port_message(opener.context_id, channel_id, opener.port_id, json);
        return;
    }
    if (channel->opening) {
        if (channel->queued_messages.size() == 1024) {
            close_channel(channel_id, "Port message queue is full"_string);
            return;
        }
        channel->queued_messages.append(move(json));
        return;
    }
    Vector<PortEndpoint> targets;
    for (auto const& [id, receiver] : channel->receivers)
        targets.append(receiver);
    for (auto const& target : targets) {
        if (owns_port(*target.endpoint, target.context_id, channel_id, target.port_id))
            target.endpoint->dispatch_extension_port_message(target.context_id, channel_id, target.port_id, json);
    }
}

void ExtensionMessageBroker::forget_port(u64 channel_id, PortEndpoint const& port)
{
    auto contexts = m_contexts.get(port.endpoint);
    if (!contexts.has_value())
        return;
    if (auto context = contexts->get(port.context_id); context.has_value())
        context->ports.remove(channel_id);
}

void ExtensionMessageBroker::close_channel(u64 channel_id, Optional<String> error, bool notify_opener)
{
    auto channel = m_channels.take(channel_id);
    if (!channel.has_value())
        return;
    forget_port(channel_id, channel->opener);
    for (auto const& [id, receiver] : channel->receivers)
        forget_port(channel_id, receiver);
    auto notify = [&](PortEndpoint const& port) {
        auto contexts = m_contexts.get(port.endpoint);
        if (contexts.has_value() && contexts->contains(port.context_id))
            port.endpoint->dispatch_extension_port_disconnect(port.context_id, port.port_id, error);
    };
    if (notify_opener)
        notify(channel->opener);
    for (auto const& [id, receiver] : channel->receivers) {
        if (receiver.dispatched)
            notify(receiver);
    }
}

void ExtensionMessageBroker::remove_port(u64 channel_id, PortEndpoint const& port)
{
    auto channel = m_channels.get(channel_id);
    if (!channel.has_value())
        return;
    if (channel->opener.endpoint == port.endpoint && channel->opener.context_id == port.context_id && channel->opener.port_id == port.port_id) {
        close_channel(channel_id, {}, false);
        return;
    }
    auto receiver = channel->receivers.take(port.port_id);
    if (!receiver.has_value())
        return;
    forget_port(channel_id, *receiver);
    channel->had_receiver |= receiver->dispatched;
    if (!receiver->acknowledged)
        --channel->outstanding;
    if (channel->opening)
        finish_opening(channel_id);
    else if (channel->receivers.is_empty())
        close_channel(channel_id);
}

void ExtensionMessageBroker::port_disconnect(Endpoint& endpoint, u64 context_id, u64 channel_id, u64 port_id)
{
    if (owns_port(endpoint, context_id, channel_id, port_id))
        remove_port(channel_id, { &endpoint, context_id, port_id });
}

void ExtensionMessageBroker::close_channels_for(String const& extension_id)
{
    Vector<u64> channels;
    for (auto const& [id, channel] : m_channels) {
        if (channel.extension_id == extension_id)
            channels.append(id);
    }
    for (auto id : channels)
        close_channel(id);
}

bool ExtensionMessageBroker::has_open_channels(String const& extension_id) const
{
    for (auto const& [id, channel] : m_channels) {
        if (channel.extension_id != extension_id)
            continue;
        if (channel.opener.background)
            return true;
        for (auto const& [port_id, receiver] : channel.receivers) {
            if (receiver.background)
                return true;
        }
    }
    return false;
}

}
