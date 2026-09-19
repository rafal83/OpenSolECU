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
std::vector<Record> intradayHistoryAll(Resolution resolution, uint64_t start, uint64_t end) {
    auto inventory = configInventory();
    struct Bucket {
        std::array<double, maxChannels> power{};
        std::array<bool, maxChannels> have{};
        uint8_t channelCount = 0;
    };
    std::map<uint64_t, Bucket> byTimestamp;
    for (size_t i = 0; i < inventory.count; i++) {
        const char *serial = inventory.entries[i].serial;
        storageVisit(resolution, [&](const Record &r) {
            if (!strcmp(r.serial, serial) && r.timestamp >= start && r.timestamp < end) {
                auto &b = byTimestamp[r.timestamp];
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
    rows.reserve(byTimestamp.size());
    for (auto &entry : byTimestamp) {
        Record r;
        r.timestamp = entry.first;
        r.resolution = resolution;
        r.channelCount = entry.second.channelCount;
        for (size_t channel = 0; channel < r.channelCount; channel++)
            r.channels[channel] =
                entry.second.have[channel] ? float(entry.second.power[channel]) : missing;
        rows.push_back(r);
    }
    return rows;
}
bool consolidatedDay(int32_t day, const char *serial, Record &out) {
    auto primary = configGet();
    if (!serial)
        serial = primary.serial;
    time_t now = time(nullptr);
    // Today's Day record is only flushed to flash on rollover to the next day (see
    // Aggregator::add), so today must be synthesized from the live running totals instead.
    if (day == dayKey(now)) {
        auto t = fleetTotals(serial);
        if (t.day != day)
            return false;
        out = {};
        strncpy(out.serial, serial, 12);
        out.timestamp = dayStart(now);
        out.day = t.day;
        out.resolution = Resolution::Day;
        out.energyWh = t.dayWh;
        out.peak = t.peak;
        out.peakTime = t.peakTime;
        return true;
    }
    bool found = false;
    storageVisit(Resolution::Day, [&](const Record &r) {
        if (!strcmp(r.serial, serial) && r.day == day) {
            out = r;
            found = true;
        }
        return !found;
    });
    return found;
}
bool consolidatedDayAll(int32_t day, Record &out) {
    auto inventory = configInventory();
    time_t now = time(nullptr);
    double totalWh = 0;
    bool any = false;
    if (day == dayKey(now)) {
        for (size_t i = 0; i < inventory.count; i++) {
            auto t = fleetTotals(inventory.entries[i].serial);
            if (t.day == day) {
                totalWh += t.dayWh;
                any = true;
            }
        }
    } else {
        for (size_t i = 0; i < inventory.count; i++) {
            const char *serial = inventory.entries[i].serial;
            bool found = false;
            storageVisit(Resolution::Day, [&](const Record &r) {
                if (!strcmp(r.serial, serial) && r.day == day) {
                    totalWh += r.energyWh;
                    any = found = true;
                }
                return !found;
            });
        }
    }
    if (!any)
        return false;
    out = {};
    out.day = day;
    out.timestamp = dayStartFromKey(day);
    out.resolution = Resolution::Day;
    out.energyWh = totalWh;
    out.peak = missing;
    return true;
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
