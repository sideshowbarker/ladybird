/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <glib-object.h>

namespace Ladybird {

// RAII wrapper for a GObject-derived pointer. Takes ownership at construction and calls g_object_unref on the
// pointer when destroyed. Move-only (copies would need an extra g_object_ref; keep that explicit at call sites).
//
// Used in the ATK accessibility bridge so that AtkObject-typed members don't leak when AtkBridge is destroyed.
// The usual pattern is "make something", "give caller a ref via g_object_ref", "hold our own ref via GObjectPtr":
//
//     GObjectPtr<AtkObject> m_document_root;
//     m_document_root = GObjectPtr<AtkObject>::adopt(ladybird_atk_object_new(...));
template<typename T>
class GObjectPtr {
public:
    GObjectPtr() = default;

    GObjectPtr(GObjectPtr const&) = delete;
    GObjectPtr& operator=(GObjectPtr const&) = delete;

    GObjectPtr(GObjectPtr&& other) noexcept
        : m_ptr(other.m_ptr)
    {
        other.m_ptr = nullptr;
    }

    GObjectPtr& operator=(GObjectPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    ~GObjectPtr() { reset(); }

    // Take ownership of an existing reference; no additional g_object_ref is performed. The caller must have
    // a reference to transfer.
    static GObjectPtr adopt(T* ptr)
    {
        GObjectPtr wrapper;
        wrapper.m_ptr = ptr;
        return wrapper;
    }

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    // Release ownership without unref; caller becomes responsible for the reference.
    [[nodiscard]] T* release()
    {
        auto* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

    void reset(T* new_ptr = nullptr)
    {
        if (m_ptr)
            g_object_unref(m_ptr);
        m_ptr = new_ptr;
    }

private:
    T* m_ptr { nullptr };
};

}
