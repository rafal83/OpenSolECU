#pragma once
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sol.hpp"
#include <string>
namespace sol {
class Guard {
    SemaphoreHandle_t mutex_;

  public:
    explicit Guard(SemaphoreHandle_t m) : mutex_(m) {
        xSemaphoreTake(m, portMAX_DELAY);
    }
    ~Guard() {
        xSemaphoreGive(mutex_);
    }
};
struct Config {
    uint32_t version = 2;
    char installation[49] = "OpenSolECU", ssid[33] = {}, password[65] = {};
    char apSsid[33] = {}, apPassword[65] = {};
    bool apEnabled = true;
    char timezone[65] = "Europe/Paris", serial[13] = {}, ecu[13] = "00124B000001";
    uint16_t inverterId = 0, pan = 0xa3d8;
    uint8_t channel = 20, logLevel = 2;
    uint32_t pollSeconds = 5;
    bool sniffer = true;
};
constexpr size_t maxInverters = 16;
struct InverterConfig {
    char serial[13] = {}, name[33] = {};
    uint16_t address = 0;
    InverterModel model = InverterModel::Auto;
    uint8_t reserved = 0;
};
struct Inventory {
    uint32_t version = 2, count = 0;
    std::array<InverterConfig, maxInverters> entries{};
};
Inventory configInventory();
bool configBegin();
bool configResetCredentials();
Config configGet();
bool configUpdate(const cJSON *json, std::string &error);
bool configSetInverterId(uint16_t address);
cJSON *configJson();
} // namespace sol
