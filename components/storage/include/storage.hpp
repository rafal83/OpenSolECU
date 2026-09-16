#pragma once
#include "config.hpp"
namespace sol {
bool storageBegin();
bool storageRestore(Record &r);
bool storageEnqueue(const Record &r);
bool storageVisit(Resolution res, const std::function<bool(const Record &)> &fn);
cJSON *storageJson();
} // namespace sol
