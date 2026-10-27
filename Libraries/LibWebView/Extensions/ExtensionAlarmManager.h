/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebView/Export.h>

namespace WebView {

// An alarm as alarms.get() reports it: its scheduled time in milliseconds since the epoch.
struct ExtensionAlarm {
    String name;
    double scheduled_time { 0 };
    Optional<double> period_in_minutes;

    JsonObject to_json() const;
};

struct DueExtensionAlarm {
    String extension_id;
    ExtensionAlarm alarm;
};

// Each extension's alarms, in memory only, so none outlast the browser. The caller passes the time, and asks for the
// alarms that are due whenever next_due() comes.
class WEBVIEW_API ExtensionAlarmManager {
public:
    static constexpr size_t maximum_alarms_per_extension = 500;
    static constexpr size_t maximum_name_length = 1024;
    // An unpacked extension's alarms may come as often as this, and no more often.
    static constexpr i64 minimum_delay_milliseconds = 1000;

    struct CreateInfo {
        Optional<double> when;
        Optional<double> delay_in_minutes;
        Optional<double> period_in_minutes;
    };

    // Creates an alarm, replacing any of the same name. Extensions compare the errors' wording.
    ErrorOr<void, String> create(String const& extension_id, String name, CreateInfo, UnixDateTime now);
    Optional<ExtensionAlarm> get(StringView extension_id, StringView name) const;
    Vector<ExtensionAlarm> get_all(StringView extension_id) const;
    bool clear(StringView extension_id, StringView name);
    bool clear_all(StringView extension_id);
    void drop_extension(StringView extension_id);

    Optional<UnixDateTime> next_due() const;
    // The alarms due by now, each once: A periodic one is scheduled again for its next period in line with its first
    // time, and any other is gone.
    Vector<DueExtensionAlarm> take_due(UnixDateTime now);

private:
    struct Entry {
        ExtensionAlarm alarm;
        i64 fires_at { 0 };
    };
    HashMap<String, Vector<Entry>> m_alarms;
};

}
