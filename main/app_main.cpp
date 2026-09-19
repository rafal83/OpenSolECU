#include "acquisition.hpp"
#include "config.hpp"
#include "esp_ota_ops.h"
#include "fleet.hpp"
#include "freertos/task.h"
void serialConsoleBegin();
#include "logging.hpp"
#include "sniffer.hpp"
#include "storage.hpp"
#include "time_manager.hpp"
#include "updater.hpp"
#include "web_server.hpp"
#include "wifi_manager.hpp"
extern "C" void app_main() {
    if (!sol::configBegin()) {
        printf("Configuration initialization failed. NVS was preserved.\n");
        return;
    }
    sol::logBegin();
    sol::log(2, "OpenSolECU starting");
    if (!sol::storageBegin() || !sol::snifferBegin() || !sol::wifiBegin()) {
        sol::log(0, "Service initialization failed");
        return;
    }
    sol::timeBegin();
    if (!sol::fleetBegin() || !sol::acquisitionBegin() || !sol::webBegin() || !sol::updaterBegin()) {
        sol::log(0, "Application initialization failed");
        return;
    }
    serialConsoleBegin();
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_ota_mark_app_valid_cancel_rollback();
}
