#include "logging.hpp"
#include "esp_timer.h"
#include <cstdarg>
#include <cstdio>
namespace sol {
struct Entry {
    uint64_t ms;
    unsigned level;
    char text[320];
};
static Ring<Entry, 64> entries;
static SemaphoreHandle_t mutex;
void logBegin() {
    mutex = xSemaphoreCreateMutex();
}
void log(unsigned level, const char *fmt, ...) {
    if (level > configGet().logLevel)
        return;
    Entry e{};
    e.ms = esp_timer_get_time() / 1000;
    e.level = level;
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    {
        Guard g(mutex);
        entries.push(e);
    }
    if (level < 3)
        printf("[%llu] %s\n", e.ms, e.text);
}
cJSON *logJson() {
    auto a = cJSON_CreateArray();
    Guard g(mutex);
    for (size_t i = 0; i < entries.size(); i++) {
        auto &e = entries.at(i);
        auto j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "ms", e.ms);
        cJSON_AddNumberToObject(j, "level", e.level);
        cJSON_AddStringToObject(j, "message", e.text);
        cJSON_AddItemToArray(a, j);
    }
    return a;
}
} // namespace sol
