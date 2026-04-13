/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>

namespace Ladybird {

// Manages a private dbus-daemon instance for the ATK accessibility bridge. The private bus keeps our ATK bridge
// isolated from Qt's built-in QSpiAccessibleBridge (which runs on the normal AT-SPI2 bus), avoiding the
// two-providers-on-one-bus corruption that occurs when both bridges share the same bus.
class PrivateAccessibilityBus {
public:
    PrivateAccessibilityBus();
    ~PrivateAccessibilityBus();

    PrivateAccessibilityBus(PrivateAccessibilityBus const&) = delete;
    PrivateAccessibilityBus& operator=(PrivateAccessibilityBus const&) = delete;

    // Spawn a private dbus-daemon and return its address. The address is also written to a well-known file so the
    // Orca script can discover it. Returns empty Optional on failure.
    Optional<ByteString> start();

    // Stop the private dbus-daemon and remove the address file.
    void stop();

    ByteString const& address() const { return m_address; }
    bool is_running() const { return m_watchdog_pid > 0; }

    // Path where the bus address is written for the Orca script to read.
    static ByteString address_file_path();

private:
    // dbus-daemon is not our direct child. A watchdog middleman is: we fork it, it forks dbus-daemon.
    // The watchdog stays in our SELinux domain (no exec) so PDEATHSIG works against it, and it holds the
    // read end of m_watchdog_heartbeat_fd. Closing the write end (whether we exit cleanly or crash) makes
    // the watchdog's blocking read return EOF, at which point it SIGTERMs dbus-daemon and exits. This is
    // the only reliable mechanism on SELinux-enforcing systems because the exec of dbus-daemon triggers a
    // domain transition to unconfined_dbusd_t, and the kernel clears pdeath_signal on transitions that
    // set bprm->secureexec.
    ByteString m_address;
    pid_t m_watchdog_pid { -1 };
    int m_watchdog_heartbeat_fd { -1 };
};

}
