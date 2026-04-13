/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Accessibility/Linux/PrivateAccessibilityBus.h>

#include <gio/gio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

StringView view_of(ByteString const& s) { return s.view(); }

}

TEST_CASE(private_bus_starts_with_unix_path_address)
{
    Ladybird::PrivateAccessibilityBus bus;
    auto address = bus.start();

    EXPECT(address.has_value());
    EXPECT(bus.is_running());
    EXPECT(!address.value().is_empty());
    EXPECT(view_of(address.value()).starts_with("unix:"sv));

    bus.stop();
    EXPECT(!bus.is_running());
}

TEST_CASE(private_bus_writes_and_removes_address_file)
{
    auto path = Ladybird::PrivateAccessibilityBus::address_file_path();

    // Precondition: no stale file from a previous aborted test.
    unlink(path.characters());

    Ladybird::PrivateAccessibilityBus bus;
    auto address = bus.start();
    EXPECT(address.has_value());

    // File exists and contains the same address string.
    EXPECT_EQ(access(path.characters(), F_OK), 0);

    FILE* f = fopen(path.characters(), "r");
    EXPECT_NE(f, nullptr);
    char buffer[512] = {};
    auto bytes = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    EXPECT(bytes > 0);
    auto buffer_view = StringView(buffer, bytes);
    EXPECT_EQ(buffer_view, view_of(address.value()));

    bus.stop();

    // Stop removes the file.
    EXPECT_EQ(access(path.characters(), F_OK), -1);
}

TEST_CASE(private_bus_accepts_gdbus_client_connection_and_responds_to_list_names)
{
    Ladybird::PrivateAccessibilityBus bus;
    auto address = bus.start();
    EXPECT(address.has_value());

    GError* error = nullptr;
    auto* connection = g_dbus_connection_new_for_address_sync(
        address.value().characters(),
        static_cast<GDBusConnectionFlags>(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    EXPECT_EQ(error, nullptr);
    EXPECT_NE(connection, nullptr);

    // ListNames is the cheapest self-describing call that exercises the full round trip.
    auto* result = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames",
        nullptr, G_VARIANT_TYPE("(as)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    EXPECT_EQ(error, nullptr);
    EXPECT_NE(result, nullptr);
    if (result)
        g_variant_unref(result);

    // Also verify the daemon advertises its own name — Hello is implicit on bus connections, so
    // GetId is a safer probe that doesn't need the client to hold any particular name.
    auto* id_result = g_dbus_connection_call_sync(connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetId",
        nullptr, G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error);
    EXPECT_EQ(error, nullptr);
    EXPECT_NE(id_result, nullptr);
    if (id_result) {
        gchar const* id = nullptr;
        g_variant_get(id_result, "(&s)", &id);
        EXPECT_NE(id, nullptr);
        EXPECT(strlen(id) > 0);
        g_variant_unref(id_result);
    }

    g_object_unref(connection);
    bus.stop();
}

TEST_CASE(private_bus_restart_produces_new_address)
{
    Ladybird::PrivateAccessibilityBus bus;
    auto first = bus.start();
    EXPECT(first.has_value());
    bus.stop();

    auto second = bus.start();
    EXPECT(second.has_value());
    // The dbus-daemon --print-address with --address='unix:tmpdir=X' allocates a fresh socket path on
    // each start, so the two addresses must differ.
    EXPECT(view_of(first.value()) != view_of(second.value()));
    bus.stop();
}

// Verify the watchdog architecture reaps dbus-daemon when the parent process dies abruptly — the whole reason
// PrivateAccessibilityBus exists as a separate mechanism rather than relying on PR_SET_PDEATHSIG alone. Fork a child,
// have it start a bus, SIGKILL the child, then confirm the unix socket that was serving that bus no longer exists on
// disk (the watchdog SIGTERMs dbus-daemon, which removes its listener socket during clean shutdown).
TEST_CASE(private_bus_is_reaped_when_parent_process_is_sigkilled)
{
    // Pipe for the child to hand us back the address it got from its bus.
    int address_pipe[2];
    EXPECT_EQ(pipe(address_pipe), 0);

    pid_t child = fork();
    EXPECT(child >= 0);

    if (child == 0) {
        close(address_pipe[0]);
        Ladybird::PrivateAccessibilityBus bus;
        auto address = bus.start();
        if (address.has_value()) {
            auto written = write(address_pipe[1], address.value().characters(), address.value().length());
            (void)written;
        }
        close(address_pipe[1]);
        // Sleep indefinitely; parent will SIGKILL us.
        for (;;)
            pause();
        _exit(0);
    }

    close(address_pipe[1]);
    char buffer[512] = {};
    ssize_t bytes = read(address_pipe[0], buffer, sizeof(buffer) - 1);
    close(address_pipe[0]);
    EXPECT(bytes > 0);

    auto address = StringView(buffer, bytes);

    // Extract the unix socket path from the bus address so we can check whether dbus-daemon removed it.
    auto path_key = "unix:path="sv;
    auto path_start = address.find(path_key);
    EXPECT(path_start.has_value());
    auto after_key = address.substring_view(*path_start + path_key.length());
    auto end = after_key.find(',').value_or(after_key.length());
    auto socket_path = ByteString(after_key.substring_view(0, end));

    EXPECT_EQ(access(socket_path.characters(), F_OK), 0);

    // Now SIGKILL the child. Its destructors do not run. Only the watchdog's pipe-EOF handling can
    // clean up dbus-daemon and its listener socket.
    EXPECT_EQ(kill(child, SIGKILL), 0);
    int status = 0;
    waitpid(child, &status, 0);

    // Give the watchdog a generous window to wake from read() and SIGTERM dbus-daemon, and for
    // dbus-daemon to unlink its socket during graceful shutdown.
    bool socket_removed = false;
    for (int i = 0; i < 60; ++i) {
        if (access(socket_path.characters(), F_OK) != 0) {
            socket_removed = true;
            break;
        }
        usleep(50 * 1000);
    }

    EXPECT(socket_removed);

    // Best-effort cleanup in case the socket somehow wasn't removed — don't leave artifacts in
    // /run/user/<uid>/ that other tests could pick up.
    if (!socket_removed)
        unlink(socket_path.characters());
}

// Verify the stale-address-file sweep that runs at the top of PrivateAccessibilityBus::start(). This is the
// belt-and-suspenders path for the rare case where both Ladybird and its watchdog die without being able to clean up
// (e.g., both OOM-killed). It scans XDG_RUNTIME_DIR for any ladybird-a11y-<pid>.address file whose owning Ladybird PID
// is no longer alive, unlinks the unix socket referenced by each stale address, and unlinks the address file itself.
// The logic is silent and branchy — easy to regress if the address format, filename pattern, or parsing changes,
// which is exactly why we pin the behaviour down here.
TEST_CASE(start_sweeps_stale_address_files_from_dead_processes)
{
    // Fork + immediately reap to obtain a PID we know is not currently running. Linux may reuse
    // the PID eventually, but the window between waitpid() and the sweep below is short enough
    // that it's effectively always dead during the test.
    pid_t ephemeral = fork();
    EXPECT(ephemeral >= 0);
    if (ephemeral == 0)
        _exit(0);
    int wait_status = 0;
    waitpid(ephemeral, &wait_status, 0);

    char const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !runtime_dir[0])
        runtime_dir = "/tmp";

    // Fabricate an orphaned socket file and an address file pointing at it.
    auto socket_path = ByteString::formatted("{}/ladybird-a11y-test-sock-{}", runtime_dir, getpid());
    auto stale_address_file = ByteString::formatted("{}/ladybird-a11y-{}.address", runtime_dir, ephemeral);

    {
        FILE* f = fopen(socket_path.characters(), "w");
        EXPECT_NE(f, nullptr);
        if (f) {
            fputs("stub\n", f);
            fclose(f);
        }
    }
    {
        FILE* f = fopen(stale_address_file.characters(), "w");
        EXPECT_NE(f, nullptr);
        if (f) {
            fprintf(f, "unix:path=%s,guid=testguid\n", socket_path.characters());
            fclose(f);
        }
    }

    EXPECT_EQ(access(socket_path.characters(), F_OK), 0);
    EXPECT_EQ(access(stale_address_file.characters(), F_OK), 0);

    // start() invokes cleanup_stale_address_files() internally before spawning the new bus. Our
    // own file (named after getpid()) is left alone because getpid() is alive.
    Ladybird::PrivateAccessibilityBus bus;
    auto address = bus.start();
    EXPECT(address.has_value());

    // The sweep unlinks the stale address file and the orphaned socket.
    EXPECT_NE(access(stale_address_file.characters(), F_OK), 0);
    EXPECT_NE(access(socket_path.characters(), F_OK), 0);

    bus.stop();

    // Belt-and-suspenders: remove our fabricated files if the sweep somehow missed them, so we
    // don't leave litter for other tests.
    unlink(stale_address_file.characters());
    unlink(socket_path.characters());
}
