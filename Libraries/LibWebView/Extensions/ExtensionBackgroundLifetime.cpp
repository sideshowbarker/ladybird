/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Extensions/ExtensionBackgroundLifetime.h>

namespace WebView {

ExtensionBackgroundLifetime::ExtensionBackgroundLifetime(Kind kind, Callbacks callbacks)
    : m_kind(kind)
    , m_callbacks(move(callbacks))
{
}

void ExtensionBackgroundLifetime::set_idle_timeout(AK::Duration timeout)
{
    m_idle_timeout = timeout;
    if (m_state == State::Running)
        arm_idle_timer();
}

bool ExtensionBackgroundLifetime::wake()
{
    switch (m_state) {
    case State::Stopped:
        m_state = State::Starting;
        m_callbacks.start();
        return true;
    case State::Suspending:
        cancel_suspension();
        return false;
    case State::Starting:
    case State::Running:
        return false;
    }
    VERIFY_NOT_REACHED();
}

void ExtensionBackgroundLifetime::loaded(bool success)
{
    if (m_state != State::Starting)
        return;
    if (!success) {
        m_state = State::Stopped;
        return;
    }
    m_state = State::Running;
    arm_idle_timer();
}

void ExtensionBackgroundLifetime::activity()
{
    if (m_state == State::Suspending) {
        cancel_suspension();
        return;
    }
    if (m_state == State::Running)
        arm_idle_timer();
}

void ExtensionBackgroundLifetime::idle_timer_fired()
{
    if (m_state != State::Running || m_kind == Kind::Persistent)
        return;
    if (m_callbacks.keepalive()) {
        m_callbacks.arm_idle_timer(m_idle_timeout);
        return;
    }
    m_state = State::Suspending;
    m_callbacks.disarm_idle_timer();
    m_callbacks.arm_suspend_timer(suspend_timeout);
    m_callbacks.suspend();
}

void ExtensionBackgroundLifetime::suspend_acknowledged()
{
    if (m_state == State::Suspending)
        finish_suspension();
}

void ExtensionBackgroundLifetime::suspend_timer_fired()
{
    if (m_state == State::Suspending)
        finish_suspension();
}

void ExtensionBackgroundLifetime::stopped()
{
    m_state = State::Stopped;
    m_callbacks.disarm_idle_timer();
    m_callbacks.disarm_suspend_timer();
}

void ExtensionBackgroundLifetime::arm_idle_timer()
{
    if (m_kind == Kind::Persistent)
        return;
    m_callbacks.arm_idle_timer(m_idle_timeout);
}

void ExtensionBackgroundLifetime::cancel_suspension()
{
    m_state = State::Running;
    m_callbacks.disarm_suspend_timer();
    m_callbacks.suspend_canceled();
    arm_idle_timer();
}

void ExtensionBackgroundLifetime::finish_suspension()
{
    m_state = State::Stopped;
    m_callbacks.disarm_suspend_timer();
    m_callbacks.stop();
}

}
