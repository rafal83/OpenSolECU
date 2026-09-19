#pragma once
#include "cJSON.h"
#include "sol.hpp"
#include <vector>
namespace sol {
cJSON *statsJson(const char *serial = nullptr);
// Combined statistics summed across every configured inverter (not a single inverter's view).
cJSON *statsAllJson();
// Intraday (Minute or Quarter resolution) records within [start,end), summed per timestamp
// across every configured inverter.
std::vector<Record> intradayHistoryAll(Resolution resolution, uint64_t start, uint64_t end);
// Consolidated day-resolution rollup for one calendar day (YYYYMMDD) and one inverter. False if
// that day has no data at all (neither a flushed Day record nor, for today, any live total).
bool consolidatedDay(int32_t day, const char *serial, Record &out);
// Same, summed across every configured inverter. Peak power is left unknown (no single
// synchronized value across independently-sampled inverters), matching statsAllJson().
bool consolidatedDayAll(int32_t day, Record &out);
cJSON *recordJson(const Record &r);
} // namespace sol
