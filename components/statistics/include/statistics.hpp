#pragma once
#include "cJSON.h"
#include "sol.hpp"
#include <vector>
namespace sol {
cJSON *statsJson(const char *serial = nullptr);
// Combined statistics summed across every configured inverter (not a single inverter's view).
cJSON *statsAllJson();
std::vector<Record> dailyHistory(unsigned days, bool monthly = false, const char *serial = nullptr);
// Combined day-resolution history (energyWh summed per calendar day across every configured inverter).
std::vector<Record> dailyHistoryAll(unsigned days, bool monthly = false);
// Combined minute-resolution history for today (same channel indexes summed across inverters).
std::vector<Record> minuteHistoryAllToday();
cJSON *recordJson(const Record &r);
} // namespace sol
