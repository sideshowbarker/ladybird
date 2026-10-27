/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibThreading/Thread.h>
#include <LibWeb/Extensions/ExtensionSet.h>

using namespace Web::Extensions;

static ExtensionDescriptor descriptor(StringView id)
{
    ExtensionDescriptor descriptor;
    descriptor.id = MUST(String::from_utf8(id));
    return descriptor;
}

// A worker runs on a thread of the page's process, and Hub sends each worker the enabled extensions over the worker's
// own connection, which that thread handles. So every thread keeps a set of its own: the main thread's readers iterate
// theirs while a worker thread replaces its own, and neither sees the other's.
TEST_CASE(each_thread_has_a_set_of_its_own)
{
    ExtensionSet::the().set_extensions({ descriptor("main"sv) });

    bool main_set_seen_on_thread = true;
    size_t count_on_thread = 0;
    // The thread is joined below, so the pointers outlive it.
    auto thread = MUST(Threading::Thread::try_create("extension-set"sv, [seen = &main_set_seen_on_thread, count = &count_on_thread]() -> intptr_t {
        *seen = ExtensionSet::the().find("main"sv) != nullptr;
        ExtensionSet::the().set_extensions({ descriptor("worker-a"sv), descriptor("worker-b"sv) });
        *count = ExtensionSet::the().extensions().size();
        return 0;
    }));
    thread->start();
    (void)thread->join();

    EXPECT(!main_set_seen_on_thread);
    EXPECT_EQ(count_on_thread, 2u);
    EXPECT_EQ(ExtensionSet::the().extensions().size(), 1u);
    EXPECT(ExtensionSet::the().find("main"sv) != nullptr);
    EXPECT(ExtensionSet::the().find("worker-a"sv) == nullptr);
}
