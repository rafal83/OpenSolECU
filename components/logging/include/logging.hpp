#pragma once
#include "config.hpp"
namespace sol {
void logBegin();
void log(unsigned level, const char *fmt, ...);
cJSON *logJson();
} // namespace sol
