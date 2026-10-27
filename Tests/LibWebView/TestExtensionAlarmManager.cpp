/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibWebView/Extensions/ExtensionAlarmManager.h>

using WebView::ExtensionAlarmManager;

static UnixDateTime at(i64 milliseconds) { return UnixDateTime::from_milliseconds_since_epoch(milliseconds); }
static constexpr i64 now = 1'000'000;

static Vector<String> names_of(Vector<WebView::DueExtensionAlarm> const& due)
{
    Vector<String> names;
    for (auto const& alarm : due)
        names.append(alarm.alarm.name);
    return names;
}

TEST_CASE(a_delay_under_a_second_is_raised_to_one)
{
    ExtensionAlarmManager alarms;
    MUST(alarms.create("ext"_string, "a"_string, { .when = {}, .delay_in_minutes = 0.001, .period_in_minutes = {} }, at(now)));
    EXPECT_EQ(alarms.get("ext"sv, "a"sv)->scheduled_time, now + 1000.0);
    EXPECT(alarms.take_due(at(now + 999)).is_empty());
    EXPECT_EQ(names_of(alarms.take_due(at(now + 1000))), (Vector { "a"_string }));
    EXPECT(!alarms.get("ext"sv, "a"sv).has_value());
}

TEST_CASE(a_periodic_alarm_repeats_in_line_with_its_first_time)
{
    ExtensionAlarmManager alarms;
    MUST(alarms.create("ext"_string, "p"_string, { .when = {}, .delay_in_minutes = {}, .period_in_minutes = 1 }, at(now)));
    EXPECT_EQ(alarms.get("ext"sv, "p"sv)->scheduled_time, now + 60'000.0);
    // Two periods went by unseen, so it fires once, and next at the third.
    EXPECT_EQ(alarms.take_due(at(now + 200'000)).size(), 1u);
    EXPECT_EQ(alarms.get("ext"sv, "p"sv)->scheduled_time, now + 240'000.0);
    EXPECT_EQ(alarms.get("ext"sv, "p"sv)->period_in_minutes, 1.0);
}

TEST_CASE(an_alarm_comes_due_once)
{
    ExtensionAlarmManager alarms;
    MUST(alarms.create("ext"_string, "a"_string, { .when = now + 5000.0, .delay_in_minutes = {}, .period_in_minutes = {} }, at(now)));
    EXPECT_EQ(alarms.next_due(), at(now + 5000));
    EXPECT_EQ(alarms.take_due(at(now + 6000)).size(), 1u);
    EXPECT(alarms.take_due(at(now + 6000)).is_empty());
    EXPECT(!alarms.next_due().has_value());
}

TEST_CASE(the_501st_alarm_is_refused_and_a_name_is_replaced)
{
    ExtensionAlarmManager alarms;
    for (size_t i = 0; i < ExtensionAlarmManager::maximum_alarms_per_extension; ++i)
        MUST(alarms.create("ext"_string, String::number(i), { .when = {}, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now)));
    auto refused = alarms.create("ext"_string, "one too many"_string, { .when = {}, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now));
    EXPECT_EQ(refused.error(), "An extension cannot have more than 500 active alarms."sv);
    MUST(alarms.create("ext"_string, "7"_string, { .when = {}, .delay_in_minutes = 2, .period_in_minutes = {} }, at(now)));
    EXPECT_EQ(alarms.get("ext"sv, "7"sv)->scheduled_time, now + 120'000.0);
    EXPECT_EQ(alarms.get_all("ext"sv).size(), 500u);
    MUST(alarms.create("other"_string, "x"_string, { .when = {}, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now)));
}

TEST_CASE(malformed_alarms_are_refused)
{
    ExtensionAlarmManager alarms;
    EXPECT_EQ(alarms.create("ext"_string, "a"_string, { .when = now + 1.0, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now)).error(), "Cannot set both when and delayInMinutes."sv);
    EXPECT_EQ(alarms.create("ext"_string, "a"_string, { .when = {}, .delay_in_minutes = {}, .period_in_minutes = {} }, at(now)).error(), "Must set at least one of when, delayInMinutes, or periodInMinutes."sv);
    EXPECT_EQ(alarms.create("ext"_string, MUST(String::repeated('n', 1025)), { .when = {}, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now)).error(), "Alarm name size is 1025 bytes which exceeds the limit of 1024 bytes."sv);
    EXPECT(alarms.get_all("ext"sv).is_empty());
}

TEST_CASE(alarms_are_cleared_and_dropped_with_the_extension)
{
    ExtensionAlarmManager alarms;
    for (auto name : { "a"sv, "b"sv, "c"sv })
        MUST(alarms.create("ext"_string, MUST(String::from_utf8(name)), { .when = {}, .delay_in_minutes = 1, .period_in_minutes = {} }, at(now)));
    EXPECT(alarms.clear("ext"sv, "b"sv));
    EXPECT(!alarms.clear("ext"sv, "b"sv));
    auto all = alarms.get_all("ext"sv);
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].name, "a"sv);
    EXPECT_EQ(all[1].name, "c"sv);
    alarms.drop_extension("ext"sv);
    EXPECT(alarms.get_all("ext"sv).is_empty());
    EXPECT(!alarms.next_due().has_value());
    EXPECT(!alarms.clear_all("ext"sv));
}

TEST_CASE(extreme_times_stay_within_the_largest_time_a_date_can_have)
{
    ExtensionAlarmManager alarms;
    static constexpr double latest = 8.64e15;
    MUST(alarms.create("ext"_string, "far"_string, { .when = {}, .delay_in_minutes = 1e20, .period_in_minutes = 1e20 }, at(now)));
    EXPECT_EQ(alarms.get("ext"sv, "far"sv)->scheduled_time, latest);
    MUST(alarms.create("ext"_string, "when"_string, { .when = 1e20, .delay_in_minutes = {}, .period_in_minutes = {} }, at(now)));
    MUST(alarms.create("ext"_string, "past"_string, { .when = -1e20, .delay_in_minutes = {}, .period_in_minutes = 0.0001 }, at(now)));
    MUST(alarms.create("ext"_string, "negative"_string, { .when = {}, .delay_in_minutes = -1e20, .period_in_minutes = {} }, at(now)));
    EXPECT_EQ(alarms.next_due(), at(now + 1000));
    // The past one repeats every second, in line with a first time it can't have had.
    EXPECT_EQ(alarms.take_due(at(now + 1000)).size(), 2u);
    EXPECT_EQ(alarms.take_due(at(static_cast<i64>(latest))).size(), 3u);
}
