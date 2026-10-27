/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/WebRequestInterest.h>

namespace {

// A local HTTP server that counts the requests it receives and answers each with an empty 200.
class CountingServer {
public:
    CountingServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            auto* raw = socket.ptr();
            m_sockets.append(move(socket));
            raw->on_ready_to_read = [this, raw] {
                auto buffer = MUST(ByteBuffer::create_uninitialized(4096));
                if (MUST(raw->read_some(buffer)).is_empty())
                    return;
                raw->on_ready_to_read = nullptr;
                ++m_requests;
                MUST(raw->set_blocking(true));
                MUST(raw->write_until_depleted("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"sv.bytes()));
                raw->close();
            };
        };
    }

    URL::URL url(StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}{}", *m_server->local_port(), path)).release_value();
    }

    size_t requests() const { return m_requests; }

private:
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Core::TCPSocket>> m_sockets;
    size_t m_requests { 0 };
};

void spin_for(Core::EventLoop& loop, AK::Duration duration)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(duration, [&] { done = true; });
    timer->start();
    loop.spin_until([&] { return done; });
}

}

// While the startup gate is on, a page's request waits before it starts, and goes on once the gate opens; a request
// that carries no page context, like the UI process's own, never waits.
TEST_CASE(startup_gate_holds_page_requests_until_released)
{
    Core::EventLoop event_loop;
    MUST(RequestServer::initialize_libcurl());

    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    auto pair = MUST(IPC::Transport::create_paired());
    auto remote_transport = MUST(pair.remote_handle.create_transport());
    auto connection = RequestServer::ConnectionFromClient::construct(
        move(pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Unrestricted, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

    CountingServer server;
    auto start = [&](u64 id, StringView path, Optional<Requests::WebRequestContext> context) {
        auto message = make<Messages::RequestServer::StartRequest>(
            id, ByteString { "GET" }, server.url(path), Vector<HTTP::Header> {}, ByteBuffer {},
            HTTP::CacheMode::NoStore, Optional<HTTP::NetworkIsolationKey> {}, HTTP::Cookie::IncludeCredentials::No, false, Optional<u32> {}, false, 0, 0, move(context));
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    };

    RequestServer::WebRequestInterestSet::the().set_startup_gate(true);
    Requests::WebRequestContext page_context;
    page_context.serial = 1;
    page_context.type = "script"_string;
    page_context.page_id = 1;
    page_context.document_url = URL::Parser::basic_parse("https://page.example/"sv);
    start(1, "/held"sv, page_context);
    start(2, "/not-a-page"sv, {});

    event_loop.spin_until([&] { return server.requests() >= 1; });
    spin_for(event_loop, AK::Duration::from_milliseconds(200));
    EXPECT_EQ(server.requests(), 1u);

    RequestServer::WebRequestInterestSet::the().set_startup_gate(false);
    RequestServer::ConnectionFromClient::release_requests_held_by_startup_gate(connections);
    event_loop.spin_until([&] { return server.requests() >= 2; });
    EXPECT_EQ(server.requests(), 2u);

    connection->shutdown();
}

// A page's WebSocket waits at startup like its other requests, before anything about it goes out.
TEST_CASE(startup_gate_holds_page_websockets_until_released)
{
    Core::EventLoop event_loop;
    MUST(RequestServer::initialize_libcurl());

    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    auto pair = MUST(IPC::Transport::create_paired());
    auto remote_transport = MUST(pair.remote_handle.create_transport());
    auto connection = RequestServer::ConnectionFromClient::construct(
        move(pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Unrestricted, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

    RequestServer::WebRequestInterestSet::the().set_startup_gate(true);
    Requests::WebRequestContext page_context;
    page_context.serial = 1;
    page_context.type = "websocket"_string;
    page_context.page_id = 1;
    page_context.document_url = URL::Parser::basic_parse("https://page.example/"sv);
    auto message = make<Messages::RequestServer::WebsocketConnect>(
        7, URL::Parser::basic_parse("ws://127.0.0.1:9999/socket"sv).release_value(), Optional<HTTP::NetworkIsolationKey> {}, ByteString { "https://page.example" },
        Vector<ByteString> {}, Vector<ByteString> {}, Vector<HTTP::Header> {}, Optional<Requests::WebRequestContext> { page_context });
    VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    EXPECT(connection->holds_websocket_for_startup_gate(7));

    RequestServer::WebRequestInterestSet::the().set_startup_gate(false);
    RequestServer::ConnectionFromClient::release_requests_held_by_startup_gate(connections);
    EXPECT(!connection->holds_websocket_for_startup_gate(7));

    connection->shutdown();
}
