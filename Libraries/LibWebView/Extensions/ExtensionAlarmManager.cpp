/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWebView/Extensions/ExtensionAlarmManager.h>

namespace WebView {

static constexpr double milliseconds_per_minute = 60'000;
// The largest time a Date can have; a time any later, or a period any longer, comes no sooner than this.
static constexpr double latest_time = 8.64e15;

// Times arrive as any finite number, so they're computed in doubles and only converted once they're in range.
static i64 time_from(double milliseconds, i64 earliest)
{
    return static_cast<i64>(clamp(milliseconds, static_cast<double>(earliest), latest_time));
}

static i64 period_from(double minutes)
{
    return static_cast<i64>(clamp(minutes * milliseconds_per_minute, static_cast<double>(ExtensionAlarmManager::minimum_delay_milliseconds), latest_time));
}

JsonObject ExtensionAlarm::to_json() const
{
    JsonObject object;
    object.set("name"sv, name);
    object.set("scheduledTime"sv, scheduled_time);
    if (period_in_minutes.has_value())
        object.set("periodInMinutes"sv, *period_in_minutes);
    return object;
}

ErrorOr<void, String> ExtensionAlarmManager::create(String const& extension_id, String name, CreateInfo info, UnixDateTime now)
{
    if (info.when.has_value() && info.delay_in_minutes.has_value())
        return "Cannot set both when and delayInMinutes."_string;
    if (!info.when.has_value() && !info.delay_in_minutes.has_value() && !info.period_in_minutes.has_value())
        return "Must set at least one of when, delayInMinutes, or periodInMinutes."_string;
    if (name.bytes().size() > maximum_name_length)
        return MUST(String::formatted("Alarm name size is {} bytes which exceeds the limit of {} bytes.", name.bytes().size(), maximum_name_length));

    auto& alarms = m_alarms.ensure(extension_id);
    auto existing = alarms.find_first_index_if([&](auto const& entry) { return entry.alarm.name == name; });
    if (!existing.has_value() && alarms.size() >= maximum_alarms_per_extension)
        return MUST(String::formatted("An extension cannot have more than {} active alarms.", maximum_alarms_per_extension));

    // A relative alarm comes no sooner than the minimum delay; an absolute one keeps its time but fires no sooner.
    auto now_milliseconds = now.milliseconds_since_epoch();
    auto earliest = now_milliseconds + minimum_delay_milliseconds;
    Entry entry { .alarm = { .name = move(name), .scheduled_time = 0, .period_in_minutes = info.period_in_minutes }, .fires_at = 0 };
    if (info.when.has_value()) {
        entry.alarm.scheduled_time = *info.when;
        entry.fires_at = time_from(*info.when, earliest);
    } else {
        auto delay_in_minutes = info.delay_in_minutes.value_or_lazy_evaluated([&] { return *info.period_in_minutes; });
        entry.fires_at = time_from(static_cast<double>(now_milliseconds) + delay_in_minutes * milliseconds_per_minute, earliest);
        entry.alarm.scheduled_time = static_cast<double>(entry.fires_at);
    }
    if (existing.has_value())
        alarms[*existing] = move(entry);
    else
        alarms.append(move(entry));
    return {};
}

Optional<ExtensionAlarm> ExtensionAlarmManager::get(StringView extension_id, StringView name) const
{
    auto alarms = m_alarms.get(extension_id);
    if (!alarms.has_value())
        return {};
    for (auto const& entry : *alarms) {
        if (entry.alarm.name == name)
            return entry.alarm;
    }
    return {};
}

Vector<ExtensionAlarm> ExtensionAlarmManager::get_all(StringView extension_id) const
{
    Vector<ExtensionAlarm> result;
    if (auto alarms = m_alarms.get(extension_id); alarms.has_value()) {
        for (auto const& entry : *alarms)
            result.append(entry.alarm);
    }
    return result;
}

bool ExtensionAlarmManager::clear(StringView extension_id, StringView name)
{
    auto alarms = m_alarms.find(extension_id);
    if (alarms == m_alarms.end())
        return false;
    return alarms->value.remove_first_matching([&](auto const& entry) { return entry.alarm.name == name; });
}

bool ExtensionAlarmManager::clear_all(StringView extension_id)
{
    auto alarms = m_alarms.find(extension_id);
    if (alarms == m_alarms.end() || alarms->value.is_empty())
        return false;
    alarms->value.clear();
    return true;
}

void ExtensionAlarmManager::drop_extension(StringView extension_id)
{
    m_alarms.remove(extension_id);
}

Optional<UnixDateTime> ExtensionAlarmManager::next_due() const
{
    Optional<i64> earliest;
    for (auto const& [id, alarms] : m_alarms) {
        for (auto const& entry : alarms) {
            if (!earliest.has_value() || entry.fires_at < *earliest)
                earliest = entry.fires_at;
        }
    }
    if (!earliest.has_value())
        return {};
    return UnixDateTime::from_milliseconds_since_epoch(*earliest);
}

Vector<DueExtensionAlarm> ExtensionAlarmManager::take_due(UnixDateTime now)
{
    auto now_milliseconds = now.milliseconds_since_epoch();
    Vector<DueExtensionAlarm> due;
    for (auto& [id, alarms] : m_alarms) {
        for (auto& entry : alarms) {
            if (entry.fires_at <= now_milliseconds)
                due.append({ id, entry.alarm });
        }
        alarms.remove_all_matching([&](auto& entry) {
            if (entry.fires_at > now_milliseconds)
                return false;
            if (!entry.alarm.period_in_minutes.has_value())
                return true;
            // The next period that's in line with the first scheduled time, skipping any that went by unseen.
            // Both stay within latest_time, so their arithmetic can't overflow.
            auto period = period_from(*entry.alarm.period_in_minutes);
            auto scheduled = time_from(entry.alarm.scheduled_time, 0);
            auto passed_periods = max<i64>((now_milliseconds - scheduled) / period, 0);
            entry.fires_at = scheduled + period * (passed_periods + 1);
            entry.alarm.scheduled_time = static_cast<double>(entry.fires_at);
            return false;
        });
    }
    quick_sort(due, [](auto const& a, auto const& b) { return a.alarm.scheduled_time < b.alarm.scheduled_time; });
    return due;
}

}
