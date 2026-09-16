#include "config.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstdio>
namespace sol {
static Config cfg;
static Inventory inventory;
static SemaphoreHandle_t mutex;
static bool save(const Config &c, const Inventory &inv = inventory) {
    nvs_handle_t h;
    if (nvs_open("opensolecu", NVS_READWRITE, &h) != ESP_OK)
        return false;
    auto e = nvs_set_blob(h, "config", &c, sizeof(c));
    if (e == ESP_OK)
        e = nvs_set_blob(h, "inventory", &inv, sizeof(inv));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
static const char kDefaultPassword[] = "OpenSolECU26";
bool configBegin() {
    mutex = xSemaphoreCreateMutex();
    if (!mutex || nvs_flash_init() != ESP_OK)
        return false;
    nvs_handle_t h;
    auto err = nvs_open("opensolecu", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = sizeof(cfg);
        err = nvs_get_blob(h, "config", &cfg, &n);
        if (err == ESP_OK) {
            if (n != sizeof(cfg) || cfg.version != 2) {
                nvs_close(h);
                return false;
            }
            n = sizeof(inventory);
            auto invError = nvs_get_blob(h, "inventory", &inventory, &n);
            nvs_close(h);
            if (invError != ESP_OK || n != sizeof(inventory) || inventory.version != 2 ||
                inventory.count > maxInverters) {
                inventory = {};
                return save(cfg, inventory);
            }
            return true;
        }
        nvs_close(h);
    }
    if (err != ESP_ERR_NVS_NOT_FOUND)
        return false;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(cfg.apSsid, sizeof(cfg.apSsid), "OpenSolECU-%02X%02X", mac[4], mac[5]);
    strcpy(cfg.apPassword, kDefaultPassword);
    if (!save(cfg))
        return false;
    printf("\nFIRST BOOT / PREMIER DEMARRAGE\nSSID: %s\nAP password: %s\nhttp://192.168.4.1\n", cfg.apSsid,
           cfg.apPassword);
    return true;
}
bool configResetCredentials() {
    Guard g(mutex);
    Config next = cfg;
    strcpy(next.apPassword, kDefaultPassword);
    if (!save(next))
        return false;
    cfg = next;
    printf("SSID: %s\nAP password: %s\n", cfg.apSsid, cfg.apPassword);
    return true;
}
Config configGet() {
    Guard g(mutex);
#ifdef CONFIG_OPENSOLECU_FORCE_SNIFFER
    Config passive = cfg;
    passive.sniffer = true;
    return passive;
#else
    return cfg;
#endif
}
Inventory configInventory() {
    Guard g(mutex);
    return inventory;
}
bool configUpdate(const cJSON *obj, std::string &error) {
    if (!cJSON_IsObject(obj)) {
        error = "JSON object required";
        return false;
    }
    Guard g(mutex);
    Config next = cfg;
    Inventory nextInventory = inventory;
    auto str = [&](const char *key, char *dst, size_t cap, size_t minimum = 0) -> bool {
        auto v = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (!v)
            return true;
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap || strlen(v->valuestring) < minimum) {
            error = std::string("Invalid ") + key;
            return false;
        }
        strcpy(dst, v->valuestring);
        return true;
    };
    if (!str("installation", next.installation, sizeof(next.installation), 1) ||
        !str("ssid", next.ssid, sizeof(next.ssid)) || !str("password", next.password, 64) ||
        !str("apSsid", next.apSsid, sizeof(next.apSsid), 1) || !str("apPassword", next.apPassword, 64, 8) ||
        !str("timezone", next.timezone, sizeof(next.timezone), 1) ||
        !str("serial", next.serial, sizeof(next.serial)) || !str("ecu", next.ecu, sizeof(next.ecu), 12))
        return false;
    if (next.password[0] && strlen(next.password) < 8) {
        error = "Wi-Fi password must be empty or at least 8 characters";
        return false;
    }
    uint8_t id[6];
    if (!parseHex(next.ecu, id, 6) || (*next.serial && !parseHex(next.serial, id, 6))) {
        error = "ECU and inverter serial must be 12 hexadecimal characters";
        return false;
    }
    if (strcmp(next.timezone, "Europe/Paris") && strcmp(next.timezone, "UTC") &&
        strncmp(next.timezone, "POSIX:", 6)) {
        error = "Use Europe/Paris, UTC or POSIX:<TZ rule>";
        return false;
    }
    auto number = [&](const char *key, unsigned min, unsigned max, unsigned &value) -> bool {
        auto v = cJSON_GetObjectItemCaseSensitive(obj, key);
        if (!v)
            return true;
        if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble < min ||
            v->valuedouble > max || floor(v->valuedouble) != v->valuedouble) {
            error = std::string("Invalid ") + key;
            return false;
        }
        value = v->valueint;
        return true;
    };
    unsigned val = next.pollSeconds;
    if (!number("pollSeconds", 5, 300, val))
        return false;
    next.pollSeconds = val;
    val = next.channel;
    if (!number("channel", 11, 26, val))
        return false;
    next.channel = val;
    val = next.pan;
    if (!number("pan", 1, 65534, val))
        return false;
    next.pan = val;
    val = next.inverterId;
    if (!number("inverterId", 0, 65527, val))
        return false;
    next.inverterId = val;
    val = next.logLevel;
    if (!number("logLevel", 0, 4, val))
        return false;
    next.logLevel = val;
    auto ap = cJSON_GetObjectItemCaseSensitive(obj, "apEnabled");
    if (ap) {
        if (!cJSON_IsBool(ap)) {
            error = "apEnabled must be boolean";
            return false;
        }
        next.apEnabled = cJSON_IsTrue(ap);
    }
    auto mode = cJSON_GetObjectItemCaseSensitive(obj, "mode");
    if (mode) {
        if (!cJSON_IsString(mode) ||
            (strcmp(mode->valuestring, "NORMAL") && strcmp(mode->valuestring, "SNIFFER"))) {
            error = "mode must be NORMAL or SNIFFER";
            return false;
        }
        next.sniffer = !strcmp(mode->valuestring, "SNIFFER");
    }
#ifdef CONFIG_OPENSOLECU_FORCE_SNIFFER
    if (!next.sniffer) {
        error = "This firmware is locked to passive SNIFFER";
        return false;
    }
#endif
    auto list = cJSON_GetObjectItemCaseSensitive(obj, "inverters");
    if (list) {
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > int(maxInverters)) {
            error = "inverters must be an array of at most 16 APsystems inverters";
            return false;
        }
        nextInventory = {};
        const cJSON *entry;
        cJSON_ArrayForEach(entry, list) {
            auto serial = cJSON_GetObjectItemCaseSensitive(entry, "serial");
            auto name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            auto address = cJSON_GetObjectItemCaseSensitive(entry, "address");
            auto model = cJSON_GetObjectItemCaseSensitive(entry, "model");
            InverterModel parsedModel = InverterModel::Auto;
            uint8_t bytes[6];
            if (!cJSON_IsObject(entry) || !cJSON_IsString(serial) ||
                !parseHex(serial->valuestring, bytes, 6) ||
                (name && (!cJSON_IsString(name) || strlen(name->valuestring) > 32)) ||
                (model && (!cJSON_IsString(model) ||
                           !parseInverterModel(model->valuestring, parsedModel))) ||
                (address &&
                 (!cJSON_IsNumber(address) || address->valuedouble < 0 || address->valuedouble > 65527 ||
                  floor(address->valuedouble) != address->valuedouble))) {
                error = "Invalid inverter serial, name or address";
                return false;
            }
            auto &v = nextInventory.entries[nextInventory.count];
            for (int k = 0; k < 6; k++)
                snprintf(v.serial + k * 2, 3, "%02X", bytes[k]);
            for (size_t k = 0; k < nextInventory.count; k++)
                if (!strcmp(v.serial, nextInventory.entries[k].serial)) {
                    error = "Duplicate inverter serial";
                    return false;
                }
            if (name)
                strcpy(v.name, name->valuestring);
            v.address = address ? address->valueint : 0;
            v.model = parsedModel;
            ++nextInventory.count;
        }
        strcpy(next.serial, nextInventory.count ? nextInventory.entries[0].serial : "");
        next.inverterId = nextInventory.count ? nextInventory.entries[0].address : 0;
    } else if (cJSON_GetObjectItemCaseSensitive(obj, "serial") ||
               cJSON_GetObjectItemCaseSensitive(obj, "inverterId")) {
        if (*next.serial) {
            parseHex(next.serial, id, 6);
            for (int i = 0; i < 6; i++)
                snprintf(next.serial + 2 * i, 3, "%02X", id[i]);
            if (!nextInventory.count)
                nextInventory.count = 1;
            strcpy(nextInventory.entries[0].serial, next.serial);
            nextInventory.entries[0].address = next.inverterId;
            for (size_t i = 1; i < nextInventory.count; i++)
                if (!strcmp(next.serial, nextInventory.entries[i].serial)) {
                    error = "Duplicate inverter serial";
                    return false;
                }
        } else if (nextInventory.count) {
            error = "Use inverters to remove an inverter from the inventory";
            return false;
        }
    }
    if (!save(next, nextInventory)) {
        error = "NVS commit failed";
        return false;
    }
    cfg = next;
    inventory = nextInventory;
    return true;
}
bool configSetInverterId(uint16_t a) {
    Guard g(mutex);
    Config next = cfg;
    next.inverterId = a;
    auto inv = inventory;
    if (inv.count)
        inv.entries[0].address = a;
    if (!save(next, inv))
        return false;
    cfg = next;
    inventory = inv;
    return true;
}
cJSON *configJson() {
    auto c = configGet();
    auto j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "version", c.version);
    cJSON_AddBoolToObject(j, "authenticationRequired", false);
#ifdef CONFIG_OPENSOLECU_FORCE_SNIFFER
    cJSON_AddBoolToObject(j, "normalModeAvailable", false);
#else
    cJSON_AddBoolToObject(j, "normalModeAvailable", true);
#endif
    cJSON_AddStringToObject(j, "installation", c.installation);
    cJSON_AddStringToObject(j, "ssid", c.ssid);
    cJSON_AddStringToObject(j, "apSsid", c.apSsid);
    cJSON_AddBoolToObject(j, "apEnabled", c.apEnabled);
    cJSON_AddStringToObject(j, "timezone", c.timezone);
    cJSON_AddStringToObject(j, "serial", c.serial);
    cJSON_AddStringToObject(j, "ecu", c.ecu);
    cJSON_AddNumberToObject(j, "inverterId", c.inverterId);
    cJSON_AddNumberToObject(j, "pan", c.pan);
    cJSON_AddNumberToObject(j, "channel", c.channel);
    cJSON_AddNumberToObject(j, "pollSeconds", c.pollSeconds);
    cJSON_AddNumberToObject(j, "logLevel", c.logLevel);
    auto list = cJSON_AddArrayToObject(j, "inverters");
    auto inv = configInventory();
    for (size_t i = 0; i < inv.count; i++) {
        auto v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "serial", inv.entries[i].serial);
        cJSON_AddStringToObject(v, "name", inv.entries[i].name);
        cJSON_AddNumberToObject(v, "address", inv.entries[i].address);
        cJSON_AddStringToObject(v, "model", inverterModelName(inv.entries[i].model));
        cJSON_AddItemToArray(list, v);
    }
    cJSON_AddNumberToObject(j, "maxInverters", maxInverters);
    return j;
}
} // namespace sol
