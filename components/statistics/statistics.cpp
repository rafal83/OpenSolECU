#include "statistics.hpp"
#include "acquisition.hpp"
#include "fleet.hpp"
#include "storage.hpp"
#include <algorithm>
#include <map>
namespace sol {
static void number(cJSON *j, const char *k, double v) {
    if (std::isfinite(v))
        cJSON_AddNumberToObject(j, k, v);
    else
        cJSON_AddNullToObject(j, k);
}
cJSON *recordJson(const Record &r) {
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "serial", r.serial);
    number(j, "timestamp", r.timestamp);
    number(j, "day", r.day);
    number(j, "resolution", int(r.resolution));
    number(j, "duration", r.duration);
    number(j, "coverage", r.coverage);
    cJSON_AddNumberToObject(j, "channelCount", r.channelCount);
    auto channels = cJSON_AddArrayToObject(j, "channels");
    double totalPower = 0;
    bool havePower = false;
    for (size_t i = 0; i < r.channelCount; i++) {
        if (std::isfinite(r.channels[i])) {
            cJSON_AddItemToArray(channels, cJSON_CreateNumber(r.channels[i]));
            totalPower += r.channels[i];
            havePower = true;
        } else
            cJSON_AddItemToArray(channels, cJSON_CreateNull());
    }
    number(j, "pv1", r.channels[0]);
    number(j, "pv2", r.channels[1]);
    number(j, "totalPower", havePower ? totalPower : missing);
    number(j, "energyWh", r.energyWh);
    number(j, "totalWh", r.totalWh);
    number(j, "dayWh", r.dayWh);
    number(j, "peak", r.peak);
    number(j, "peakTime", r.peakTime);
    cJSON_AddBoolToObject(j, "simulated", r.flags & 1);
    return j;
}
std::vector<Record> dailyHistory(unsigned days, bool monthly, const char *serial) {
    auto primary = configGet();
    if (!serial)
        serial = primary.serial;
    std::vector<Record> rows;
    time_t now = time(nullptr);
    if (now < 1704067200)
        return rows;
    tm start{};
    localtime_r(&now, &start);
    start.tm_hour = start.tm_min = start.tm_sec = 0;
    if (monthly) {
        start.tm_mday = 1;
        start.tm_mon -= 11;
    } else
        start.tm_mday -= days - 1;
    start.tm_isdst = -1;
    time_t from = mktime(&start);
    storageVisit(Resolution::Day, [&](const Record &r) {
        if (!strcmp(r.serial, serial) && r.timestamp >= uint64_t(from) && r.timestamp <= uint64_t(now))
            rows.push_back(r);
        return true;
    });
    auto t = fleetTotals(serial);
    if (t.day == dayKey(now)) {
        Record today;
        strncpy(today.serial, serial, 12);
        today.timestamp = dayStart(now);
        today.day = t.day;
        today.resolution = Resolution::Day;
        today.energyWh = t.dayWh;
        today.peak = t.peak;
        today.peakTime = t.peakTime;
        rows.push_back(today);
    }
    std::sort(rows.begin(), rows.end(),
              [](const Record &a, const Record &b) { return a.timestamp < b.timestamp; });
    // Only measured days are returned. Missing days never become fabricated zero energy.
    if (monthly) {
        std::vector<Record> months;
        for (auto &r : rows) {
            int key = r.day / 100;
            if (months.empty() || months.back().day != key) {
                Record m;
                strncpy(m.serial, serial, 12);
                m.day = key;
                time_t epoch = r.timestamp;
                tm date{};
                localtime_r(&epoch, &date);
                date.tm_mday = 1;
                date.tm_isdst = -1;
                m.timestamp = mktime(&date);
                m.resolution = Resolution::Day;
                months.push_back(m);
            }
            months.back().energyWh += r.energyWh;
        }
        return months;
    }
    return rows;
}
std::vector<Record> dailyHistoryAll(unsigned days, bool monthly) {
    auto inventory = configInventory();
    std::vector<Record> rows;
    time_t now = time(nullptr);
    if (now < 1704067200)
        return rows;
    tm start{};
    localtime_r(&now, &start);
    start.tm_hour = start.tm_min = start.tm_sec = 0;
    if (monthly) {
        start.tm_mday = 1;
        start.tm_mon -= 11;
    } else
        start.tm_mday -= days - 1;
    start.tm_isdst = -1;
    time_t from = mktime(&start);
    std::map<uint64_t, double> byTimestamp;
    std::map<uint64_t, int32_t> dayOf;
    for (size_t i = 0; i < inventory.count; i++) {
        const char *serial = inventory.entries[i].serial;
        storageVisit(Resolution::Day, [&](const Record &r) {
            if (!strcmp(r.serial, serial) && r.timestamp >= uint64_t(from) && r.timestamp <= uint64_t(now)) {
                byTimestamp[r.timestamp] += r.energyWh;
                dayOf[r.timestamp] = r.day;
            }
            return true;
        });
        auto t = fleetTotals(serial);
        if (t.day == dayKey(now)) {
            uint64_t ts = dayStart(now);
            byTimestamp[ts] += t.dayWh;
            dayOf[ts] = t.day;
        }
    }
    for (auto &entry : byTimestamp) {
        Record r;
        r.timestamp = entry.first;
        r.day = dayOf[entry.first];
        r.resolution = Resolution::Day;
        r.energyWh = entry.second;
        rows.push_back(r);
    }
    // byTimestamp is a std::map, so rows are already ordered by timestamp.
    if (monthly) {
        std::vector<Record> months;
        for (auto &r : rows) {
            int key = r.day / 100;
            if (months.empty() || months.back().day != key) {
                Record m;
                m.day = key;
                time_t epoch = r.timestamp;
                tm date{};
                localtime_r(&epoch, &date);
                date.tm_mday = 1;
                date.tm_isdst = -1;
                m.timestamp = mktime(&date);
                m.resolution = Resolution::Day;
                months.push_back(m);
            }
            months.back().energyWh += r.energyWh;
        }
        return months;
    }
    return rows;
}
std::vector<Record> minuteHistoryAllToday() {
    auto inventory = configInventory();
    uint64_t start = dayStart(time(nullptr));
    struct Bucket {
        std::array<double, maxChannels> power{};
        std::array<bool, maxChannels> have{};
        uint8_t channelCount = 0;
    };
    std::map<uint64_t, Bucket> byMinute;
    for (size_t i = 0; i < inventory.count; i++) {
        const char *serial = inventory.entries[i].serial;
        storageVisit(Resolution::Minute, [&](const Record &r) {
            if (!strcmp(r.serial, serial) && r.timestamp >= start) {
                auto &b = byMinute[r.timestamp];
                b.channelCount = std::max(b.channelCount, r.channelCount);
                for (size_t channel = 0; channel < r.channelCount; channel++) {
                    if (std::isfinite(r.channels[channel])) {
                        b.power[channel] += r.channels[channel];
                        b.have[channel] = true;
                    }
                }
            }
            return true;
        });
    }
    std::vector<Record> rows;
    rows.reserve(byMinute.size());
    for (auto &entry : byMinute) {
        Record r;
        r.timestamp = entry.first;
        r.resolution = Resolution::Minute;
        r.channelCount = entry.second.channelCount;
        for (size_t channel = 0; channel < r.channelCount; channel++)
            r.channels[channel] =
                entry.second.have[channel] ? float(entry.second.power[channel]) : missing;
        rows.push_back(r);
    }
    return rows;
}
cJSON *statsJson(const char *serial) {
    auto primary = configGet();
    if (!serial)
        serial = primary.serial;
    StatisticsAccumulator accumulator(fleetTotals(serial), time(nullptr));
    storageVisit(Resolution::Day, [&](const Record &r) {
        if (!strcmp(r.serial, serial))
            accumulator.add(r);
        return true;
    });
    auto t = accumulator.result();
    if (!t.completedDays && fleetTotals(serial).day == 0) {
        t.totalWh = t.weekWh = t.monthWh = t.yearWh = missing;
    }
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "serial", serial);
    number(j, "todayWh", t.todayWh);
    number(j, "totalWh", t.totalWh);
    number(j, "peakToday", t.peakToday);
    number(j, "peakTime", t.peakTime);
    number(j, "yesterdayWh", t.yesterdayWh);
    number(j, "weekWh", t.weekWh);
    number(j, "monthWh", t.monthWh);
    number(j, "yearWh", t.yearWh);
    number(j, "bestDayWh", t.bestDayWh);
    number(j, "bestDay", t.bestDay);
    number(j, "dailyAverageWh", t.averageWh);
    number(j, "completedDays", t.completedDays);
    cJSON_AddStringToObject(j, "scope", "measured records; unknown intervals excluded");
    return j;
}
cJSON *statsAllJson() {
    // Combines every configured inverter's own per-day energy into one calendar-day series before
    // running the same day-by-day accumulation statsJson() uses for a single inverter, so
    // today/yesterday/week/month/year/best-day/average are exact sums, not per-inverter
    // approximations. "Today's peak power" has no single well-defined value across
    // independently-sampled inverters (each can peak at a different instant), so it is left
    // unknown here rather than presented as a misleading number.
    auto inventory = configInventory();
    std::map<int32_t, double> byDay;
    double totalWh = 0, todayWh = 0;
    int32_t today = dayKey(time(nullptr));
    bool anyToday = false;
    for (size_t i = 0; i < inventory.count; i++) {
        const char *serial = inventory.entries[i].serial;
        auto t = fleetTotals(serial);
        totalWh += t.totalWh;
        if (t.day == today) {
            todayWh += t.dayWh;
            anyToday = true;
        }
        storageVisit(Resolution::Day, [&](const Record &r) {
            if (!strcmp(r.serial, serial) && r.day < today)
                byDay[r.day] += r.energyWh;
            return true;
        });
    }
    StatisticsAccumulator accumulator(Totals{totalWh, todayWh, missing, 0, anyToday ? today : 0},
                                      time(nullptr));
    for (auto &entry : byDay) {
        Record r;
        r.resolution = Resolution::Day;
        r.day = entry.first;
        r.energyWh = entry.second;
        accumulator.add(r);
    }
    auto t = accumulator.result();
    if (!t.completedDays && !anyToday)
        t.totalWh = t.weekWh = t.monthWh = t.yearWh = missing;
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "serial", "ALL");
    cJSON_AddBoolToObject(j, "combined", true);
    cJSON_AddNumberToObject(j, "inverterCount", inventory.count);
    number(j, "todayWh", t.todayWh);
    number(j, "totalWh", t.totalWh);
    number(j, "peakToday", t.peakToday);
    number(j, "peakTime", t.peakTime);
    number(j, "yesterdayWh", t.yesterdayWh);
    number(j, "weekWh", t.weekWh);
    number(j, "monthWh", t.monthWh);
    number(j, "yearWh", t.yearWh);
    number(j, "bestDayWh", t.bestDayWh);
    number(j, "bestDay", t.bestDay);
    number(j, "dailyAverageWh", t.averageWh);
    number(j, "completedDays", t.completedDays);
    cJSON_AddStringToObject(j, "scope",
                            "measured records summed across all configured inverters; unknown intervals excluded; "
                            "peak power not combined (no single synchronized value across inverters)");
    return j;
}
} // namespace sol
