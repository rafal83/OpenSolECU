#pragma once
#include "cJSON.h"
namespace sol {
bool wifiBegin();
cJSON *wifiJson();
cJSON *wifiScan();
} // namespace sol
