#pragma once
#include "config.hpp"
#include "sniffer_core.hpp"
namespace sol {
bool fleetBegin();
void fleetCapture(const CapturedFrame &frame);
void fleetPublish(const char *serial, const InverterState &state);
InverterState fleetState(const char *serial);
Totals fleetTotals(const char *serial);
cJSON *fleetJson();
} // namespace sol
