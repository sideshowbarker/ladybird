/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/OwnPtr.h>
#include <LibWebView/Export.h>

namespace WebView {

struct ExtensionStorageChange {
    Optional<JsonValue> old_value;
    Optional<JsonValue> new_value;
};
using StorageChanges = OrderedHashMap<String, ExtensionStorageChange>;

class WEBVIEW_API ExtensionStorageStore {
    AK_ALLOC_WITH_KMALLOC;

public:
    static constexpr u64 quota_bytes = 10485760;
    using Writer = Function<ErrorOr<void>(ByteString const&, String const&)>;
    explicit ExtensionStorageStore(ByteString profile_data, Writer = {});
    // storage.session's store keeps each extension's items in memory alone, under the same quota, which
    // unlimitedStorage doesn't lift.
    static NonnullOwnPtr<ExtensionStorageStore> create_session_store();
    ~ExtensionStorageStore();
    void get(String const&, Optional<Vector<String>>, Function<void(ErrorOr<JsonObject>)>);
    void set(String const&, JsonObject, bool unlimited, Function<void(ErrorOr<StorageChanges>)>);
    void remove(String const&, Vector<String>, Function<void(ErrorOr<StorageChanges>)>);
    void clear(String const&, Function<void(ErrorOr<StorageChanges>)>);
    void bytes_in_use(String const&, Optional<Vector<String>>, Function<void(ErrorOr<u64>)>);
    void delete_storage(String const&, Function<void(ErrorOr<void>)>);

private:
    struct Impl;
    OwnPtr<Impl> m_impl;
};

}
