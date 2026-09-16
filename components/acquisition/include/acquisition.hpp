#pragma once
#include "cJSON.h"
#include "sol.hpp"
namespace sol {
bool acquisitionBegin();
InverterState liveState();
Totals liveTotals();
cJSON *liveJson();
cJSON *acquisitionJson();
bool requestPair();
bool passiveTxSelfTest();
} // namespace sol
