#include "wifi_manager.hpp"
#include "config.hpp"
#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "logging.hpp"
#include "mdns.h"
#include <atomic>
namespace sol {
static esp_netif_t *apNet, *staNet;
static std::atomic<bool> connected{false}, connecting{false}, scanPending{false};
static std::atomic<unsigned> failures{0};
static uint32_t retryAt = 0;
static wifi_ap_record_t networks[24];
static uint16_t networkCount = 0;
static SemaphoreHandle_t scanMutex;
static void event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected = true;
        connecting = false;
        failures = 0;
        log(2, "Home Wi-Fi connected; direct AP stays available");
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        connecting = false;
        failures++;
        log(1, "Home Wi-Fi disconnected; retry scheduled");
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        Guard g(scanMutex);
        networkCount = 24;
        if (esp_wifi_scan_get_ap_records(&networkCount, networks) != ESP_OK)
            networkCount = 0;
        scanPending = false;
    }
}
static void worker(void *) {
    bool apOn = true;
    for (;;) {
        auto c = configGet();
        bool desired = WifiPolicy::apActive(c.apEnabled, connected);
        if (desired != apOn) {
            if (esp_wifi_set_mode(desired ? WIFI_MODE_APSTA : WIFI_MODE_STA) == ESP_OK)
                apOn = desired;
        }
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (*c.ssid && !connected && !connecting && !scanPending && int32_t(now - retryAt) >= 0) {
            connecting = true;
            if (esp_wifi_connect() != ESP_OK)
                connecting = false;
            retryAt = now + WifiPolicy::retryDelay(failures);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
bool wifiBegin() {
    scanMutex = xSemaphoreCreateMutex();
    if (!scanMutex)
        return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK)
        return false;
    apNet = esp_netif_create_default_wifi_ap();
    staNet = esp_netif_create_default_wifi_sta();
    if (!apNet || !staNet)
        return false;
    esp_netif_set_hostname(apNet, "opensolecu");
    esp_netif_set_hostname(staNet, "opensolecu");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK)
        return false;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event, nullptr));
    auto c = configGet();
    wifi_config_t ap{}, sta{};
    memcpy(ap.ap.ssid, c.apSsid, strlen(c.apSsid));
    ap.ap.ssid_len = strlen(c.apSsid);
    strcpy(reinterpret_cast<char *>(ap.ap.password), c.apPassword);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.capable = true;
    strcpy(reinterpret_cast<char *>(sta.sta.ssid), c.ssid);
    strcpy(reinterpret_cast<char *>(sta.sta.password), c.password);
    sta.sta.threshold.authmode = *c.password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK || esp_wifi_start() != ESP_OK)
        return false;
    // The IEEE 802.15.4 radio (snifferBegin) is already enabled by this point; without this,
    // Wi-Fi/802.15.4 coexistence is half-configured and Wi-Fi clients cannot associate.
    esp_coex_wifi_i154_enable();
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (mdns_init() != ESP_OK)
        return false;
    mdns_hostname_set("opensolecu");
    mdns_instance_name_set("OpenSolECU");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    return xTaskCreate(worker, "wifi", 4096, nullptr, 3, nullptr) == pdPASS;
}
static void ipFields(cJSON *j, esp_netif_t *net) {
    esp_netif_ip_info_t ip{};
    esp_netif_get_ip_info(net, &ip);
    char b[16];
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.ip));
    cJSON_AddStringToObject(j, "ip", b);
    snprintf(b, sizeof(b), IPSTR, IP2STR(&ip.gw));
    cJSON_AddStringToObject(j, "gateway", b);
}
cJSON *wifiJson() {
    auto c = configGet();
    auto j = cJSON_CreateObject();
    auto ap = cJSON_AddObjectToObject(j, "ap");
    auto sta = cJSON_AddObjectToObject(j, "sta");
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    wifi_sta_list_t clients{};
    esp_wifi_ap_get_sta_list(&clients);
    cJSON_AddBoolToObject(ap, "enabled", mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP);
    cJSON_AddStringToObject(ap, "ssid", c.apSsid);
    cJSON_AddNumberToObject(ap, "clients", clients.num);
    ipFields(ap, apNet);
    cJSON_AddBoolToObject(sta, "connected", connected);
    cJSON_AddBoolToObject(sta, "connecting", connecting);
    cJSON_AddStringToObject(sta, "ssid", c.ssid);
    ipFields(sta, staNet);
    wifi_ap_record_t info{};
    if (esp_wifi_sta_get_ap_info(&info) == ESP_OK)
        cJSON_AddNumberToObject(sta, "rssi", info.rssi);
    else
        cJSON_AddNullToObject(sta, "rssi");
    cJSON_AddStringToObject(j, "hostname", "opensolecu.local");
    return j;
}
cJSON *wifiScan() {
    // Start asynchronously: the HTTP server remains responsive while scanning.
    if (!scanPending && !connecting) {
        scanPending = true;
        wifi_scan_config_t scan{};
        scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan.scan_time.active.min = 30;
        scan.scan_time.active.max = 80;
        if (esp_wifi_scan_start(&scan, false) != ESP_OK)
            scanPending = false;
    }
    auto j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "scanning", scanPending);
    auto a = cJSON_AddArrayToObject(j, "networks");
    Guard g(scanMutex);
    for (unsigned i = 0; i < networkCount; i++) {
        auto v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "ssid", reinterpret_cast<const char *>(networks[i].ssid));
        cJSON_AddNumberToObject(v, "rssi", networks[i].rssi);
        cJSON_AddNumberToObject(v, "channel", networks[i].primary);
        cJSON_AddBoolToObject(v, "secured", networks[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(a, v);
    }
    return j;
}
} // namespace sol
