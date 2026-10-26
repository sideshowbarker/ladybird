/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWebView/Extensions/ExtensionBackgroundLifetime.h>

using WebView::ExtensionBackgroundLifetime;
using State = ExtensionBackgroundLifetime::State;

namespace {

// Records what the lifetime asks of its host, and stands in for its timers.
struct Host {
    explicit Host(ExtensionBackgroundLifetime::Kind kind)
        : lifetime(kind, callbacks())
    {
    }

    ExtensionBackgroundLifetime::Callbacks callbacks()
    {
        return {
            .start = [this] { ++starts; },
            .suspend = [this] { ++suspends; },
            .stop = [this] { ++stops; },
            .suspend_canceled = [this] { ++cancels; },
            .arm_idle_timer = [this](AK::Duration) { idle_timer_armed = true; },
            .disarm_idle_timer = [this] { idle_timer_armed = false; },
            .arm_suspend_timer = [this](AK::Duration) { suspend_timer_armed = true; },
            .disarm_suspend_timer = [this] { suspend_timer_armed = false; },
            .keepalive = [this] { return keepalive; },
        };
    }

    void run_until_running()
    {
        lifetime.wake();
        lifetime.loaded(true);
    }

    int starts { 0 };
    int suspends { 0 };
    int stops { 0 };
    int cancels { 0 };
    bool idle_timer_armed { false };
    bool suspend_timer_armed { false };
    bool keepalive { false };
    ExtensionBackgroundLifetime lifetime;
};

}

TEST_CASE(idle_timer_unloads_an_event_page)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    EXPECT_EQ(host.lifetime.state(), State::Running);
    EXPECT(host.idle_timer_armed);
    host.lifetime.idle_timer_fired();
    EXPECT_EQ(host.lifetime.state(), State::Suspending);
    EXPECT_EQ(host.suspends, 1);
    EXPECT(host.suspend_timer_armed);
    host.lifetime.suspend_acknowledged();
    EXPECT_EQ(host.lifetime.state(), State::Stopped);
    EXPECT_EQ(host.stops, 1);
    EXPECT(!host.suspend_timer_armed);
}

TEST_CASE(an_unanswered_suspend_still_stops_the_page)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.lifetime.idle_timer_fired();
    host.lifetime.suspend_timer_fired();
    EXPECT_EQ(host.lifetime.state(), State::Stopped);
    EXPECT_EQ(host.stops, 1);
}

TEST_CASE(persistent_never_unloads)
{
    Host host(ExtensionBackgroundLifetime::Kind::Persistent);
    host.run_until_running();
    EXPECT(!host.idle_timer_armed);
    host.lifetime.activity();
    EXPECT(!host.idle_timer_armed);
    EXPECT_EQ(host.lifetime.state(), State::Running);
}

TEST_CASE(activity_resets_the_timer)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.idle_timer_armed = false;
    host.lifetime.activity();
    EXPECT(host.idle_timer_armed);
}

TEST_CASE(activity_during_suspend_cancels)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.lifetime.idle_timer_fired();
    host.lifetime.activity();
    EXPECT_EQ(host.lifetime.state(), State::Running);
    EXPECT_EQ(host.cancels, 1);
    EXPECT(!host.suspend_timer_armed);
    EXPECT(host.idle_timer_armed);
    // A late acknowledgment of the canceled suspension stops nothing.
    host.lifetime.suspend_acknowledged();
    EXPECT_EQ(host.lifetime.state(), State::Running);
    EXPECT_EQ(host.stops, 0);
}

TEST_CASE(keepalive_holds_the_page)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.keepalive = true;
    for (int i = 0; i < 100; ++i)
        host.lifetime.idle_timer_fired();
    EXPECT_EQ(host.lifetime.state(), State::Running);
    EXPECT(host.idle_timer_armed);
}

TEST_CASE(a_manifest_version_3_keepalive_holds_while_the_host_says_so)
{
    // The host bounds each unanswered message itself, so the lifetime keeps the page as long as the host asks.
    Host host(ExtensionBackgroundLifetime::Kind::NonPersistentPage);
    host.run_until_running();
    host.keepalive = true;
    for (int i = 0; i < 100; ++i)
        host.lifetime.idle_timer_fired();
    EXPECT_EQ(host.lifetime.state(), State::Running);
    host.keepalive = false;
    host.lifetime.idle_timer_fired();
    EXPECT_EQ(host.lifetime.state(), State::Suspending);
}

TEST_CASE(concurrent_wakes_start_once)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    EXPECT(host.lifetime.wake());
    EXPECT(!host.lifetime.wake());
    EXPECT_EQ(host.starts, 1);
    EXPECT_EQ(host.lifetime.state(), State::Starting);
}

TEST_CASE(stop_then_wake_starts_again)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.lifetime.idle_timer_fired();
    host.lifetime.suspend_acknowledged();
    EXPECT(host.lifetime.wake());
    EXPECT_EQ(host.starts, 2);
    host.lifetime.loaded(true);
    EXPECT_EQ(host.lifetime.state(), State::Running);
}

TEST_CASE(waking_a_suspending_page_cancels_the_suspension)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.lifetime.idle_timer_fired();
    EXPECT(!host.lifetime.wake());
    EXPECT_EQ(host.lifetime.state(), State::Running);
    EXPECT_EQ(host.cancels, 1);
    EXPECT_EQ(host.starts, 1);
}

TEST_CASE(a_failed_load_stops_and_a_forced_stop_disarms)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.lifetime.wake();
    host.lifetime.loaded(false);
    EXPECT_EQ(host.lifetime.state(), State::Stopped);
    host.run_until_running();
    host.lifetime.stopped();
    EXPECT_EQ(host.lifetime.state(), State::Stopped);
    EXPECT(!host.idle_timer_armed);
}

TEST_CASE(the_pages_own_activity_doesnt_cancel_its_suspension)
{
    Host host(ExtensionBackgroundLifetime::Kind::EventPage);
    host.run_until_running();
    host.lifetime.idle_timer_fired();
    host.lifetime.activity(ExtensionBackgroundLifetime::FromPage::Yes);
    EXPECT_EQ(host.lifetime.state(), State::Suspending);
    EXPECT_EQ(host.cancels, 0);
    host.lifetime.suspend_acknowledged();
    EXPECT_EQ(host.lifetime.state(), State::Stopped);
}
