#include "acquisition.hpp"
#include "config.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sniffer.hpp"
#include "storage.hpp"
#include "wifi_manager.hpp"
#include <cstdio>
#include <cstdlib>
#include <sys/time.h>
static void reply(cJSON *j) {
    // Single right-sized buffer rather than cJSON_PrintUnformatted's realloc-and-copy growth
    // (sniffer-frames responses are large enough for that pattern to fragment the heap).
    char *text = cJSON_PrintBuffered(j, 8192, false);
    cJSON_Delete(j);
    if (text) {
        printf("JSON:%s\n", text);
        cJSON_free(text);
    }
}
static void command(char *line) {
    if (!strcmp(line, "credentials-reset")) {
        if (sol::configResetCredentials()) {
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else
            printf("RESULT:credentials reset failed\n");
        return;
    }
    if (!strcmp(line, "sniffer-frames")) {
        reply(sol::snifferJson(0, 256));
        return;
    }
    if (!strcmp(line, "status")) {
        auto j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "firmware", "OpenSolECU");
        cJSON_AddStringToObject(j, "idf", esp_get_idf_version());
        cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(j, "minimumHeap", esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(j, "uptime", esp_timer_get_time() / 1000000);
        cJSON_AddItemToObject(j, "wifi", sol::wifiJson());
        cJSON_AddItemToObject(j, "radio", sol::acquisitionJson());
        cJSON_AddItemToObject(j, "storage", sol::storageJson());
        reply(j);
        return;
    }
    if (!strcmp(line, "sniffer")) {
        reply(sol::snifferJson(0, 0));
        return;
    }
    if (!strncmp(line, "sniffer ", 8)) {
        auto j = cJSON_Parse(line + 8);
        bool ok = sol::snifferControl(j);
        cJSON_Delete(j);
        printf("RESULT:sniffer %s\n", ok ? "accepted" : "rejected");
        return;
    }
    if (!strcmp(line, "tx-guard-test")) {
        printf("RESULT:tx-guard %s\n", sol::passiveTxSelfTest() ? "PASS" : "UNAVAILABLE");
        return;
    }
    if (!strcmp(line, "wifi-scan")) {
        reply(sol::wifiScan());
        return;
    }
    if (!strncmp(line, "wifi ", 5)) {
        auto j = cJSON_Parse(line + 5);
        auto clean = cJSON_CreateObject();
        for (const char *key : {"ssid", "password"}) {
            auto v = cJSON_GetObjectItemCaseSensitive(j, key);
            if (cJSON_IsString(v))
                cJSON_AddStringToObject(clean, key, v->valuestring);
        }
        std::string error;
        bool ok = cJSON_IsObject(j) && cJSON_GetArraySize(clean) > 0 && sol::configUpdate(clean, error);
        cJSON_Delete(j);
        cJSON_Delete(clean);
        printf("RESULT:wifi %s\n", ok ? "saved; rebooting" : error.c_str());
        if (ok) {
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        return;
    }
    if (!strncmp(line, "time ", 5)) {
        char *end;
        long long epoch = strtoll(line + 5, &end, 10);
        if (!*end && epoch >= 1704067200 && epoch <= 4102444800LL) {
            timeval tv{time_t(epoch), 0};
            settimeofday(&tv, nullptr);
            printf("RESULT:time set\n");
        }
        return;
    }
    printf("COMMANDS: status | credentials-reset | sniffer | sniffer-frames | sniffer {JSON control} | "
           "tx-guard-test | wifi-scan | wifi {ssid,password} | time <UTC epoch>\n");
}
static void consoleTask(void *) {
    char line[512];
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        uint8_t b;
        int n = usb_serial_jtag_read_bytes(&b, 1, pdMS_TO_TICKS(100));
        if (n != 1)
            continue;
        if (b == '\r' || b == '\n') {
            if (!overflow && used) {
                line[used] = 0;
                command(line);
            }
            used = 0;
            overflow = false;
        } else if (used + 1 < sizeof(line))
            line[used++] = b;
        else
            overflow = true;
    }
}
void serialConsoleBegin() {
    usb_serial_jtag_driver_config_t config{1024, 1024};
    if (usb_serial_jtag_driver_install(&config) == ESP_OK)
        xTaskCreate(consoleTask, "serial", 8192, nullptr, 2, nullptr);
}
