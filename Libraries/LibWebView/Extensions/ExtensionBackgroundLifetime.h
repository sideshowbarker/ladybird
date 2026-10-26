/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Time.h>
#include <LibWebView/Export.h>

namespace WebView {

// When an extension's background page runs. A persistent page runs for as long as the extension is enabled. Any other
// page unloads once it has had nothing to do for the idle timeout, and starts again when something needs it. The host
// owns the page and the timers, and tells the lifetime what happens to them.
class WEBVIEW_API ExtensionBackgroundLifetime {
    AK_ALLOC_WITH_KMALLOC;

public:
    enum class Kind : u8 {
        Persistent,
        // A manifest version 2 page with "persistent": false, which an open port keeps loaded.
        EventPage,
        // A manifest version 3 page, which an unanswered message keeps loaded for at most max_keepalive.
        NonPersistentPage,
    };

    enum class State : u8 {
        Stopped,
        Starting,
        Running,
        Suspending,
    };

    struct Callbacks {
        Function<void()> start;
        // Dispatches runtime.onSuspend, and later calls suspend_acknowledged() once its listeners have run.
        Function<void()> suspend;
        Function<void()> stop;
        Function<void()> suspend_canceled;
        Function<void(AK::Duration)> arm_idle_timer;
        Function<void()> disarm_idle_timer;
        Function<void(AK::Duration)> arm_suspend_timer;
        Function<void()> disarm_suspend_timer;
        // Whether something that hasn't finished, like an unanswered message or a port, holds the page. The host
        // applies each kind's limits, so that other activity can't extend them.
        Function<bool()> keepalive;
    };

    // Whether activity is the background page's own: What the page does while it's suspending doesn't keep it.
    enum class FromPage : bool {
        No,
        Yes,
    };

    static constexpr AK::Duration default_idle_timeout = AK::Duration::from_seconds(30);
    static constexpr AK::Duration suspend_timeout = AK::Duration::from_seconds(1);
    static constexpr AK::Duration max_keepalive = AK::Duration::from_seconds(300);

    ExtensionBackgroundLifetime(Kind, Callbacks);

    Kind kind() const { return m_kind; }
    State state() const { return m_state; }
    void set_idle_timeout(AK::Duration);

    // Starts a stopped page and returns true. A page that's starting or running stays as it is, and a suspending one
    // goes back to running.
    bool wake();
    void loaded(bool success);
    void activity(FromPage = FromPage::No);
    void idle_timer_fired();
    void suspend_acknowledged();
    void suspend_timer_fired();
    // The host stopped the page itself: the extension was disabled or removed, or its process went away.
    void stopped();

private:
    void arm_idle_timer();
    void cancel_suspension();
    void finish_suspension();

    Kind m_kind;
    Callbacks m_callbacks;
    State m_state { State::Stopped };
    AK::Duration m_idle_timeout { default_idle_timeout };
};

}
