#pragma once
#include "cJSON.h"
#include "radio.hpp"
#include "sniffer_core.hpp"
namespace sol {
bool snifferBegin();
void snifferRun(ESP32C6Radio &radio);
bool snifferControl(const cJSON *obj);
cJSON *snifferJson(uint64_t after = 0, size_t limit = 8);
cJSON *captureJson(const CapturedFrame &frame);
bool snifferVisit(const std::function<bool(const CapturedFrame &)> &fn);
bool snifferActive();
} // namespace sol
