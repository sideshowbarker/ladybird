/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PrivateAccessibilityBus.h"

#include <AK/Format.h>
#include <AK/StringView.h>
#include <LibCore/System.h>

#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace Ladybird {

PrivateAccessibilityBus::PrivateAccessibilityBus() = default;

PrivateAccessibilityBus::~PrivateAccessibilityBus()
{
    stop();
}

ByteString PrivateAccessibilityBus::address_file_path()
{
    // Use XDG_RUNTIME_DIR if available (typically /run/user/<uid>), otherwise fall back to /tmp.
    char const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !runtime_dir[0])
        runtime_dir = "/tmp";
    return ByteString::formatted("{}/ladybird-a11y-{}.address", runtime_dir, getpid());
}

// Unlink the unix-domain socket file referenced by the given bus address. Any orphaned dbus-daemon that
// was listening on that socket becomes reachable by nothing; the daemon itself keeps running as a harmless zombie
// until the user session ends. We accept that minor leak rather than hunt a PID across /proc, because the watchdog
// middleman is the intended cleanup path and this routine runs only as a safety net for the rare case where both
// Ladybird and its watchdog died without being able to clean up.
static void unlink_socket_for_address(StringView bus_address)
{
    auto path_key = "unix:path="sv;
    auto start = bus_address.find(path_key);
    if (!start.has_value())
        return;
    auto path_start = *start + path_key.length();
    auto remainder = bus_address.substring_view(path_start);
    auto end = remainder.find(',').value_or(remainder.length());
    auto socket_path = remainder.substring_view(0, end);

    auto null_terminated = ByteString(socket_path);
    unlink(null_terminated.characters());
}

// Remove any ladybird-a11y-<pid>.address files in XDG_RUNTIME_DIR whose owning Ladybird process is no longer
// alive. Orca's script picks the first matching file it finds, so a stale entry from a crashed prior run would
// otherwise misdirect it to a dead bus. We also unlink the unix socket referenced by the stale address — the
// normal cleanup path is the watchdog middleman, but if the watchdog itself died (rare, OOM) the daemon can outlive
// everyone.
static void cleanup_stale_address_files()
{
    char const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !runtime_dir[0])
        runtime_dir = "/tmp";

    DIR* dir = opendir(runtime_dir);
    if (!dir)
        return;

    while (auto* entry = readdir(dir)) {
        StringView name { entry->d_name, strlen(entry->d_name) };
        if (!name.starts_with("ladybird-a11y-"sv) || !name.ends_with(".address"sv))
            continue;

        auto prefix_length = "ladybird-a11y-"sv.length();
        auto suffix_length = ".address"sv.length();
        if (name.length() <= prefix_length + suffix_length)
            continue;

        auto pid_view = name.substring_view(prefix_length, name.length() - prefix_length - suffix_length);
        auto pid = pid_view.to_number<pid_t>();
        if (!pid.has_value())
            continue;

        // kill(pid, 0) is the classical liveness probe: 0 if the process exists (even as a zombie),
        // -1/ESRCH if it is gone. Any other errno means "not allowed to signal" — treat as alive.
        if (kill(*pid, 0) == 0 || errno != ESRCH)
            continue;

        auto stale_path = ByteString::formatted("{}/{}", runtime_dir, name);

        FILE* f = fopen(stale_path.characters(), "r");
        if (f) {
            char buffer[512];
            size_t bytes = fread(buffer, 1, sizeof(buffer) - 1, f);
            fclose(f);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                if (buffer[bytes - 1] == '\n')
                    buffer[bytes - 1] = '\0';
                unlink_socket_for_address(StringView { buffer, strlen(buffer) });
            }
        }

        unlink(stale_path.characters());
    }
    closedir(dir);
}

Optional<ByteString> PrivateAccessibilityBus::start()
{
    if (is_running())
        return m_address;

    // Sweep away orphaned address files (and any orphaned daemons they point at) from prior Ladybird runs
    // before advertising our own.
    cleanup_stale_address_files();

    // Build the tmpdir argument using XDG_RUNTIME_DIR for the socket.
    char const* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !runtime_dir[0])
        runtime_dir = "/tmp";
    auto address_arg = ByteString::formatted("unix:tmpdir={}", runtime_dir);

    // Two pipes:
    //   address_fds: dbus-daemon writes its --print-address line; we read it once.
    //   heartbeat_fds: the watchdog middleman reads; we hold the write end. When it closes (on clean
    //                  stop() or on Ladybird crash), the watchdog wakes and SIGTERMs dbus-daemon.
    int address_fds[2];
    if (pipe(address_fds) < 0) {
        dbgln("PrivateAccessibilityBus: pipe(address) failed: {}", strerror(errno));
        return {};
    }
    int heartbeat_fds[2];
    if (pipe(heartbeat_fds) < 0) {
        dbgln("PrivateAccessibilityBus: pipe(heartbeat) failed: {}", strerror(errno));
        close(address_fds[0]);
        close(address_fds[1]);
        return {};
    }

    pid_t watchdog_pid = fork();
    if (watchdog_pid < 0) {
        dbgln("PrivateAccessibilityBus: fork(watchdog) failed: {}", strerror(errno));
        close(address_fds[0]);
        close(address_fds[1]);
        close(heartbeat_fds[0]);
        close(heartbeat_fds[1]);
        return {};
    }

    if (watchdog_pid == 0) {
        // Watchdog middleman. Does NOT exec — it stays in Ladybird's SELinux domain, stays tiny, and runs only
        // signal-safe syscalls. Reason we need this middleman at all: when we exec dbus-daemon, SELinux transitions
        // its type to unconfined_dbusd_t, the kernel sets bprm->secureexec, and setup_new_exec() zeroes pdeath_signal
        // on the dbus-daemon process. A plain PR_SET_PDEATHSIG from Ladybird to dbus-daemon therefore never fires on
        // an SELinux-enforcing system. The watchdog avoids that by never exec'ing.

        close(address_fds[0]);   // address pipe: only dbus-daemon writes
        close(heartbeat_fds[1]); // heartbeat pipe: only Ladybird writes (we hold the read end)

        // Deliberately do NOT arm pdeath_signal on the watchdog. Default SIGTERM action is to terminate, which would
        // kill the watchdog before it gets a chance to reap dbus-daemon. The heartbeat pipe EOF is the sole
        // signalling mechanism: when Ladybird's write end is closed — either by an explicit close in stop() or by
        // the kernel releasing fds on process exit — the watchdog's blocking read() below returns 0.

        pid_t daemon_pid = fork();
        if (daemon_pid < 0) {
            _exit(127);
        }
        if (daemon_pid == 0) {
            // Grandchild: actual dbus-daemon.
            close(heartbeat_fds[0]);
            dup2(address_fds[1], STDOUT_FILENO);
            close(address_fds[1]);

            execlp("dbus-daemon", "dbus-daemon",
                "--session",
                "--nofork",
                "--print-address",
                "--address", address_arg.characters(),
                nullptr);
            _exit(127);
        }

        close(address_fds[1]);

        // Block until either (a) Ladybird closes the heartbeat write end, read returns 0 (EOF), or (b) Ladybird
        // dies abruptly, kernel closes the fd, same EOF. Then take dbus-daemon down with us. SIGTERM lets dbus-daemon
        // remove its listener socket; SIGKILL as a fallback if it ignores.
        char drain[64];
        while (true) {
            ssize_t r = read(heartbeat_fds[0], drain, sizeof(drain));
            if (r == 0)
                break;
            if (r < 0 && errno == EINTR)
                continue;
            if (r < 0)
                break;
            // Any bytes we receive here are ignored — we only care about EOF.
        }

        kill(daemon_pid, SIGTERM);
        // Give the daemon a moment to shut down cleanly, then force-kill if still around. usleep is not listed in
        // POSIX's async-signal-safe set but nanosleep is, which matters because we are post-fork in a multi-threaded
        // Qt process and must stay on signal-safe functions until _exit.
        struct timespec grace_interval { 0, 50L * 1000L * 1000L };
        for (int i = 0; i < 20; ++i) {
            int status;
            if (waitpid(daemon_pid, &status, WNOHANG) == daemon_pid)
                _exit(0);
            nanosleep(&grace_interval, nullptr);
        }
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, nullptr, 0);
        _exit(0);
    }

    // Parent.
    close(address_fds[1]);
    close(heartbeat_fds[0]);
    m_watchdog_pid = watchdog_pid;
    m_watchdog_heartbeat_fd = heartbeat_fds[1];

    char buffer[512];
    ssize_t bytes_read = read(address_fds[0], buffer, sizeof(buffer) - 1);
    close(address_fds[0]);

    if (bytes_read <= 0) {
        dbgln("PrivateAccessibilityBus: Failed to read address from dbus-daemon");
        stop();
        return {};
    }

    buffer[bytes_read] = '\0';
    if (bytes_read > 0 && buffer[bytes_read - 1] == '\n')
        buffer[bytes_read - 1] = '\0';

    m_address = ByteString(buffer);

    auto path = address_file_path();
    auto file_or_error = Core::System::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!file_or_error.is_error()) {
        auto fd = file_or_error.value();
        auto written = write(fd, m_address.characters(), m_address.length());
        (void)written;
        close(fd);
        dbgln("PrivateAccessibilityBus: Address {} written to {}", m_address, path);
    } else {
        dbgln("PrivateAccessibilityBus: Failed to write address file {}: {}", path, file_or_error.error());
    }

    return m_address;
}

void PrivateAccessibilityBus::stop()
{
    if (!is_running())
        return;

    auto path = address_file_path();
    unlink(path.characters());

    // Closing the heartbeat write end makes the watchdog's blocking read return EOF; it then SIGTERMs
    // dbus-daemon and exits. No need to signal the watchdog itself.
    if (m_watchdog_heartbeat_fd >= 0) {
        close(m_watchdog_heartbeat_fd);
        m_watchdog_heartbeat_fd = -1;
    }
    waitpid(m_watchdog_pid, nullptr, 0);

    m_watchdog_pid = -1;
    m_address = {};
}

}
