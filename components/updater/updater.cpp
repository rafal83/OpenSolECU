#include "updater.hpp"
#include "config.hpp"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include "esp_https_ota.h"
#include "freertos/task.h"
#include "logging.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
namespace sol {
namespace {
// The asset name must match what .github/workflows/release.yml uploads for this board profile.
constexpr const char *releaseUrl = "https://api.github.com/repos/rafal83/OpenSolECU/releases/latest";
constexpr const char *assetName = "opensolecu-4mb.bin";
constexpr const char *userAgent = "OpenSolECU-Updater";
constexpr uint32_t checkIntervalMs = 6u * 3600u * 1000u;
constexpr size_t maxReleaseJsonBytes = 16384;
std::atomic<bool> otaInProgress{false};
SemaphoreHandle_t stateMutex;
struct State {
    bool checked = false, available = false;
    char latestVersion[32] = {}, downloadUrl[256] = {}, error[96] = {};
    uint64_t lastCheckEpoch = 0;
} state;
// CalVer YYYY.MM.PATCH: compare field by field so month 10 sorts after month 9.
bool isNewer(const char *latest, const char *current) {
    int lp[3] = {0, 0, 0}, cp[3] = {0, 0, 0};
    sscanf(latest, "%d.%d.%d", &lp[0], &lp[1], &lp[2]);
    sscanf(current, "%d.%d.%d", &cp[0], &cp[1], &cp[2]);
    for (int i = 0; i < 3; i++)
        if (lp[i] != cp[i])
            return lp[i] > cp[i];
    return false;
}
bool fetchLatestRelease(char *outVersion, size_t versionCap, char *outUrl, size_t urlCap, char *outError,
                        size_t errorCap) {
    esp_http_client_config_t config = {};
    config.url = releaseUrl;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_agent = userAgent;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    auto client = esp_http_client_init(&config);
    if (!client) {
        snprintf(outError, errorCap, "HTTP client init failed");
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    bool ok = false;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int contentLength = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200 && contentLength > 0 && size_t(contentLength) < maxReleaseJsonBytes) {
            auto body = static_cast<char *>(malloc(size_t(contentLength) + 1));
            if (body) {
                int total = 0;
                while (total < contentLength) {
                    int n = esp_http_client_read(client, body + total, contentLength - total);
                    if (n <= 0)
                        break;
                    total += n;
                }
                body[total] = 0;
                if (total == contentLength) {
                    auto json = cJSON_Parse(body);
                    if (json) {
                        auto tag = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
                        auto assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
                        if (cJSON_IsString(tag) && cJSON_IsArray(assets)) {
                            const cJSON *asset;
                            cJSON_ArrayForEach(asset, assets) {
                                auto name = cJSON_GetObjectItemCaseSensitive(asset, "name");
                                auto url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
                                if (cJSON_IsString(name) && !strcmp(name->valuestring, assetName) &&
                                    cJSON_IsString(url)) {
                                    strncpy(outVersion, tag->valuestring, versionCap - 1);
                                    strncpy(outUrl, url->valuestring, urlCap - 1);
                                    ok = true;
                                    break;
                                }
                            }
                            if (!ok)
                                snprintf(outError, errorCap, "No %s asset in latest release", assetName);
                        } else
                            snprintf(outError, errorCap, "Unexpected release JSON shape");
                        cJSON_Delete(json);
                    } else
                        snprintf(outError, errorCap, "Malformed release JSON");
                } else
                    snprintf(outError, errorCap, "Truncated response (%d/%d)", total, contentLength);
                free(body);
            } else
                snprintf(outError, errorCap, "Out of memory");
        } else
            snprintf(outError, errorCap, "HTTP %d", status);
    } else
        snprintf(outError, errorCap, "Connection failed: %s", esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}
bool checkForUpdate() {
    char version[32] = {}, url[256] = {}, error[96] = {};
    bool found = fetchLatestRelease(version, sizeof(version), url, sizeof(url), error, sizeof(error));
    auto desc = esp_app_get_description();
    Guard g(stateMutex);
    state.checked = true;
    state.lastCheckEpoch = uint64_t(time(nullptr));
    if (found) {
        strncpy(state.latestVersion, version, sizeof(state.latestVersion) - 1);
        strncpy(state.downloadUrl, url, sizeof(state.downloadUrl) - 1);
        state.available = isNewer(version, desc->version);
        state.error[0] = 0;
        log(2, "Update check: latest=%s current=%s available=%d", version, desc->version, state.available);
    } else {
        state.available = false;
        strncpy(state.error, error, sizeof(state.error) - 1);
        log(1, "Update check failed: %s", error);
    }
    return found;
}
bool installUpdate() {
    char url[256] = {};
    {
        Guard g(stateMutex);
        if (!state.available || !state.downloadUrl[0])
            return false;
        strncpy(url, state.downloadUrl, sizeof(url) - 1);
    }
    esp_http_client_config_t httpConfig = {};
    httpConfig.url = url;
    httpConfig.crt_bundle_attach = esp_crt_bundle_attach;
    httpConfig.user_agent = userAgent;
    httpConfig.timeout_ms = 30000;
    httpConfig.keep_alive_enable = true;
    esp_https_ota_config_t otaConfig = {};
    otaConfig.http_config = &httpConfig;
    log(2, "Installing update from %s", url);
    esp_err_t err = esp_https_ota(&otaConfig);
    if (err != ESP_OK) {
        Guard g(stateMutex);
        snprintf(state.error, sizeof(state.error), "Install failed: %s", esp_err_to_name(err));
        log(0, "Update install failed: %s", esp_err_to_name(err));
        return false;
    }
    log(2, "Update installed");
    return true;
}
void updaterTask(void *) {
    vTaskDelay(pdMS_TO_TICKS(60000)); // let Wi-Fi and other services settle before the first check
    for (;;) {
        if (!otaBusy())
            checkForUpdate();
        vTaskDelay(pdMS_TO_TICKS(checkIntervalMs));
    }
}
} // namespace
bool updaterBegin() {
    stateMutex = xSemaphoreCreateMutex();
    if (!stateMutex)
        return false;
    // Generous stack: TLS handshake (mbedTLS) and JSON parsing both use deep call chains.
    return xTaskCreate(updaterTask, "updater", 8192, nullptr, 3, nullptr) == pdPASS;
}
bool updaterCheckNow() {
    if (otaBusy())
        return false;
    return checkForUpdate();
}
bool otaBeginGuard() {
    bool expected = false;
    return otaInProgress.compare_exchange_strong(expected, true);
}
void otaEndGuard() {
    otaInProgress = false;
}
bool otaBusy() {
    return otaInProgress;
}
bool updaterInstall() {
    if (!otaBeginGuard())
        return false;
    bool ok = installUpdate();
    if (!ok)
        otaEndGuard(); // on success the device reboots shortly, no need to release the guard
    return ok;
}
cJSON *updaterJson() {
    Guard g(stateMutex);
    auto j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "checked", state.checked);
    cJSON_AddBoolToObject(j, "available", state.available);
    cJSON_AddBoolToObject(j, "installing", otaBusy());
    cJSON_AddStringToObject(j, "latestVersion", state.latestVersion);
    cJSON_AddStringToObject(j, "error", state.error);
    if (state.lastCheckEpoch)
        cJSON_AddNumberToObject(j, "lastCheck", state.lastCheckEpoch);
    else
        cJSON_AddNullToObject(j, "lastCheck");
    return j;
}
} // namespace sol
