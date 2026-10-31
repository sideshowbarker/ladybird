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

namespace {

// A local server that counts the connections made to it.
class AcceptingServer {
public:
    AcceptingServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            m_sockets.append(MUST(m_server->accept()));
        };
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/", *m_server->local_port())).release_value();
    }

    size_t connections() const { return m_sockets.size(); }

private:
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Core::TCPSocket>> m_sockets;
};

void spin_for(Core::EventLoop& loop, AK::Duration duration)
{
    bool done = false;
    auto timer = Core::Timer::create_single_shot(duration, [&] { done = true; });
    timer->start();
    loop.spin_until([&] { return done; });
}

}

// With network prediction off, a page's preconnect makes no connection; turned back on, it does.
TEST_CASE(network_prediction_off_skips_preconnect)
{
    Core::EventLoop event_loop;
    MUST(RequestServer::initialize_libcurl());

    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    auto pair = MUST(IPC::Transport::create_paired());
    auto remote_transport = MUST(pair.remote_handle.create_transport());
    auto connection = RequestServer::ConnectionFromClient::construct(
        move(pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Unrestricted, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

    AcceptingServer server;
    auto preconnect = [&](u64 id) {
        auto message = make<Messages::RequestServer::EnsureConnection>(id, server.url(), RequestServer::CacheLevel::CreateConnection);
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    };

    RequestServer::ConnectionFromClient::set_network_prediction_enabled(false);
    preconnect(1);
    spin_for(event_loop, AK::Duration::from_milliseconds(300));
    EXPECT_EQ(server.connections(), 0u);

    RequestServer::ConnectionFromClient::set_network_prediction_enabled(true);
    preconnect(2);
    event_loop.spin_until([&] { return server.connections() >= 1; });
    EXPECT_EQ(server.connections(), 1u);

    connection->shutdown();
}
