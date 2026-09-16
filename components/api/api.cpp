#include "api.hpp"
#include "acquisition.hpp"
#include "config.hpp"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "logging.hpp"
#include "sniffer.hpp"
#include "statistics.hpp"
#include "storage.hpp"
#include "time_manager.hpp"
#include "wifi_manager.hpp"
#include <atomic>
#include <cstdlib>
#include <string>

namespace sol {
static std::atomic<unsigned> eventClients{0};
static std::atomic<bool> updating{false};
static esp_err_t json(httpd_req_t *req, cJSON *j, size_t prebuffer = 512) {
    if (!j)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    // A single right-sized allocation avoids cJSON_PrintUnformatted's realloc-and-copy growth,
    // which fragments an already-tight heap on the larger sniffer responses (measured:
    // minimumHeap dropped to 36 bytes under sustained /api/sniffer polling before this fix).
    char *text = cJSON_PrintBuffered(j, int(prebuffer), false);
    cJSON_Delete(j);
    if (!text)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    auto err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}
static esp_err_t error(httpd_req_t *req, const char *status, const char *message) {
    httpd_resp_set_status(req, status);
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", message);
    return json(req, j);
}
static esp_err_t ok(httpd_req_t *req, const char *message) {
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "message", message);
    return json(req, j);
}
static cJSON *body(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 4096)
        return nullptr;
    std::string data(req->content_len + 1, '\0');
    size_t offset = 0;
    while (offset < size_t(req->content_len)) {
        int n = httpd_req_recv(req, data.data() + offset, req->content_len - offset);
        if (n <= 0)
            return nullptr;
        offset += n;
    }
    return cJSON_ParseWithLengthOpts(data.c_str(), data.size(), nullptr, true);
}
static std::string query(httpd_req_t *req, const char *key, const char *fallback = "") {
    char q[256], out[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, out, sizeof(out)) != ESP_OK)
        return fallback;
    return out;
}
static bool integer(const std::string &s, uint64_t &out) {
    if (s.empty())
        return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9' || out > (UINT64_MAX - (c - '0')) / 10)
            return false;
        out = out * 10 + c - '0';
    }
    return true;
}
static cJSON *configuration() {
    auto j = configJson();
    cJSON_AddStringToObject(j, "mode", configGet().sniffer ? "SNIFFER" : "NORMAL");
    return j;
}
static cJSON *systemJson() {
    auto j = cJSON_CreateObject();
    auto desc = esp_app_get_description();
    cJSON_AddStringToObject(j, "firmware", desc->version);
    cJSON_AddStringToObject(j, "build", desc->date);
    cJSON_AddStringToObject(j, "idf", esp_get_idf_version());
    cJSON_AddNumberToObject(j, "uptimeSeconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "minimumHeap", esp_get_minimum_free_heap_size());
    cJSON_AddBoolToObject(j, "timeValid", timeValid());
    cJSON_AddStringToObject(j, "timezone", configGet().timezone);
    cJSON_AddStringToObject(j, "mode", snifferActive() ? "SNIFFER" : "NORMAL");
    cJSON_AddBoolToObject(j, "updating", updating);
    uint32_t size = 0;
    esp_flash_get_size(nullptr, &size);
    cJSON_AddNumberToObject(j, "flashBytes", size);
    cJSON_AddItemToObject(j, "wifi", wifiJson());
    cJSON_AddItemToObject(j, "live", liveJson());
    cJSON_AddItemToObject(j, "radio", acquisitionJson());
    cJSON_AddItemToObject(j, "storage", storageJson());
    return j;
}
static bool chunkJson(httpd_req_t *req, cJSON *j) {
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s)
        return false;
    auto err = httpd_resp_send_chunk(req, s, strlen(s));
    cJSON_free(s);
    return err == ESP_OK;
}
static esp_err_t history(httpd_req_t *req) {
    auto serial = query(req, "serial", configGet().serial);
    bool all = serial == "all";
    uint8_t id[6];
    if (!all && !serial.empty() && !parseHex(serial.c_str(), id, 6))
        return error(req, "400 Bad Request", "Invalid serial");
    auto range = query(req, "range", "today");
    if (range != "today" && range != "7d" && range != "30d" && range != "12m")
        return error(req, "400 Bad Request", "range must be today, 7d, 30d or 12m");
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, "{\"records\":[", 12) != ESP_OK)
        return ESP_FAIL;
    bool first = true, success = true;
    auto emit = [&](const Record &r) {
        if (!first && httpd_resp_send_chunk(req, ",", 1) != ESP_OK)
            return false;
        first = false;
        return chunkJson(req, recordJson(r));
    };
    if (range == "today") {
        if (all) {
            for (auto &r : minuteHistoryAllToday())
                if (!emit(r)) {
                    success = false;
                    break;
                }
        } else {
            uint64_t start = dayStart(time(nullptr));
            success = storageVisit(Resolution::Minute, [&](const Record &r) {
                if (!strcmp(r.serial, serial.c_str()) && r.timestamp >= start)
                    success = emit(r);
                return success;
            });
        }
    } else {
        unsigned days = range == "7d" ? 7 : range == "30d" ? 30 : 366;
        auto rows =
            all ? dailyHistoryAll(days, range == "12m") : dailyHistory(days, range == "12m", serial.c_str());
        for (auto &r : rows)
            if (!emit(r)) {
                success = false;
                break;
            }
    }
    if (!success)
        return ESP_FAIL;
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, nullptr, 0);
}
static esp_err_t csv(httpd_req_t *req) {
    auto serial = query(req, "serial", "all");
    uint64_t from = 0, to = UINT64_MAX;
    if (!integer(query(req, "from", "0"), from) || !integer(query(req, "to", "18446744073709551615"), to) ||
        from > to)
        return error(req, "400 Bad Request", "from/to must be ordered UTC Unix seconds");
    auto resolution = query(req, "resolution", "minute");
    Resolution res;
    if (resolution == "minute")
        res = Resolution::Minute;
    else if (resolution == "15min")
        res = Resolution::Quarter;
    else if (resolution == "day")
        res = Resolution::Day;
    else
        return error(req, "400 Bad Request", "resolution: minute, 15min or day");
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=opensolecu.csv");
    const char *header = "\xEF\xBB\xBFtimestamp_utc;date_local;resolution_s;pv1_W;pv2_W;pv3_W;pv4_W;energy_Wh;coverage_s;"
                         "simulated;serial\r\n";
    httpd_resp_send_chunk(req, header, strlen(header));
    bool success = true;
    storageVisit(res, [&](const Record &r) {
        if ((serial != "all" && serial != r.serial) || r.timestamp < from || r.timestamp > to)
            return true;
        char line[320], date[40], powers[maxChannels][24]{};
        time_t t = r.timestamp;
        tm local{};
        localtime_r(&t, &local);
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %z", &local);
        for (size_t channel = 0; channel < maxChannels; channel++)
            if (std::isfinite(r.channels[channel]))
                snprintf(powers[channel], sizeof(powers[channel]), "%.3f", r.channels[channel]);
        int n = snprintf(line, sizeof(line), "%llu;%s;%lu;%s;%s;%s;%s;%.6f;%lu;%u;%s\r\n", r.timestamp,
                         date, (unsigned long)r.duration, powers[0], powers[1], powers[2], powers[3],
                         r.energyWh, (unsigned long)r.coverage, r.flags & 1, r.serial);
        success = httpd_resp_send_chunk(req, line, n) == ESP_OK;
        return success;
    });
    return success ? httpd_resp_send_chunk(req, nullptr, 0) : ESP_FAIL;
}
static esp_err_t backup(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=opensolecu-backup.json");
    std::string prefix = "{\"format\":1,\"secretsIncluded\":false,\"config\":";
    httpd_resp_send_chunk(req, prefix.c_str(), prefix.size());
    if (!chunkJson(req, configuration()))
        return ESP_FAIL;
    httpd_resp_send_chunk(req, ",\"stats\":", 9);
    if (!chunkJson(req, statsJson()))
        return ESP_FAIL;
    httpd_resp_send_chunk(req, ",\"history\":[", 12);
    bool first = true, success = true;
    for (auto res : {Resolution::Minute, Resolution::Quarter, Resolution::Day}) {
        storageVisit(res, [&](const Record &r) {
            if (!first)
                httpd_resp_send_chunk(req, ",", 1);
            first = false;
            success = chunkJson(req, recordJson(r));
            return success;
        });
        if (!success)
            return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, nullptr, 0);
}
static void reboot(void *) {
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}
static void scheduleReboot() {
    xTaskCreate(reboot, "restart", 2048, nullptr, 2, nullptr);
}
static esp_err_t ota(httpd_req_t *req) {
    if (updating.exchange(true))
        return error(req, "409 Conflict", "Update already running");
    auto partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition || req->content_len <= 0 || size_t(req->content_len) > partition->size) {
        updating = false;
        return error(req, "400 Bad Request", "Firmware does not fit OTA partition");
    }
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, req->content_len, &handle);
    char buffer[2048];
    size_t remaining = req->content_len;
    while (err == ESP_OK && remaining) {
        int n = httpd_req_recv(req, buffer, std::min(remaining, sizeof(buffer)));
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(handle, buffer, n);
        remaining -= n;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(handle);
        handle = 0;
    }
    if (err == ESP_OK)
        err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        if (handle)
            esp_ota_abort(handle);
        updating = false;
        return error(req, "400 Bad Request", esp_err_to_name(err));
    }
    ok(req, "Firmware verified. Rebooting.");
    scheduleReboot();
    return ESP_OK;
}
struct Events {
    httpd_req_t *req;
    bool sniffer;
    uint64_t cursor;
};
static void eventsWorker(void *arg) {
    auto event = static_cast<Events *>(arg);
    auto req = event->req;
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    for (unsigned i = 0; i < 12; i++) {
        cJSON *j = event->sniffer ? snifferJson(event->cursor, 8) : liveJson();
        if (event->sniffer) {
            auto rows = cJSON_GetObjectItem(j, "frames");
            auto last = cJSON_GetArrayItem(rows, cJSON_GetArraySize(rows) - 1);
            if (last)
                event->cursor = cJSON_GetObjectItem(last, "id")->valuedouble;
        }
        // See json()'s comment: a single right-sized buffer, not repeated realloc growth.
        // This is the highest-frequency JSON build in the app (every 1s while a client is
        // connected), so it's the one most likely to fragment the heap over a long session.
        char *payload = cJSON_PrintBuffered(j, event->sniffer ? 8192 : 512, false);
        cJSON_Delete(j);
        if (!payload)
            break;
        const char *eventName = event->sniffer ? "frames" : "live";
        char header[64];
        int headerLength = event->sniffer
                               ? snprintf(header, sizeof(header), "event: %s\nid: %llu\ndata: ", eventName,
                                          static_cast<unsigned long long>(event->cursor))
                               : snprintf(header, sizeof(header), "event: %s\ndata: ", eventName);
        bool sent = headerLength > 0 && size_t(headerLength) < sizeof(header) &&
                    httpd_resp_send_chunk(req, header, headerLength) == ESP_OK &&
                    httpd_resp_send_chunk(req, payload, strlen(payload)) == ESP_OK &&
                    httpd_resp_send_chunk(req, "\n\n", 2) == ESP_OK;
        cJSON_free(payload);
        if (!sent)
            break;
        vTaskDelay(pdMS_TO_TICKS(event->sniffer ? 1000 : 5000));
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    httpd_req_async_handler_complete(req);
    delete event;
    --eventClients;
    vTaskDelete(nullptr);
}
static esp_err_t events(httpd_req_t *req, bool sniffer) {
    if (eventClients.fetch_add(1) >= 2) {
        --eventClients;
        return error(req, "503 Service Unavailable", "Two live streams already connected");
    }
    httpd_req_t *async = nullptr;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        --eventClients;
        return ESP_FAIL;
    }
    uint64_t cursor = 0;
    integer(query(req, "after", "0"), cursor);
    char lastId[32];
    uint64_t resume = 0;
    if (httpd_req_get_hdr_value_str(req, "Last-Event-ID", lastId, sizeof(lastId)) == ESP_OK &&
        integer(lastId, resume))
        cursor = std::max(cursor, resume);
    auto e = new Events{async, sniffer, cursor};
    if (xTaskCreate(eventsWorker, "web-events", 6144, e, 2, nullptr) != pdPASS) {
        delete e;
        --eventClients;
        httpd_req_async_handler_complete(async);
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t captureExport(httpd_req_t *req) {
    auto format = query(req, "format", "pcapng");
    if (format != "pcapng" && format != "jsonl")
        return error(req, "400 Bad Request", "format must be pcapng or jsonl");
    httpd_resp_set_type(req, format == "pcapng" ? "application/octet-stream" : "application/x-ndjson");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       format == "pcapng" ? "attachment; filename=opensolecu.pcapng"
                                          : "attachment; filename=opensolecu.jsonl");
    auto emit = [&](const uint8_t *b, size_t n) {
        return httpd_resp_send_chunk(req, reinterpret_cast<const char *>(b), n) == ESP_OK;
    };
    if (format == "pcapng" && !pcapngHeader(emit))
        return ESP_FAIL;
    bool success = snifferVisit([&](const CapturedFrame &f) {
        if (format == "pcapng")
            return pcapngFrame(f, emit);
        if (!chunkJson(req, captureJson(f)))
            return false;
        return httpd_resp_send_chunk(req, "\n", 1) == ESP_OK;
    });
    return success ? httpd_resp_send_chunk(req, nullptr, 0) : ESP_FAIL;
}
static esp_err_t handler(httpd_req_t *req) {
    std::string path(req->uri);
    path = path.substr(0, path.find('?'));
    if (req->method == HTTP_POST) {
        if (path == "/api/ota")
            return ota(req);
        auto j = body(req);
        if (!j)
            return error(req, "400 Bad Request", "Invalid JSON body");
        if (path == "/api/config" || path == "/api/wifi") {
            std::string reason;
            bool success = configUpdate(j, reason);
            cJSON_Delete(j);
            if (!success)
                return error(req, "400 Bad Request", reason.c_str());
            ok(req, "Configuration saved. Rebooting to apply network and radio mode.");
            scheduleReboot();
            return ESP_OK;
        }
        if (path == "/api/sniffer/control") {
            bool success = snifferControl(j);
            cJSON_Delete(j);
            return success ? ok(req, "Passive capture command queued")
                           : error(req, "409 Conflict", "Sniffer inactive or invalid capture command");
        }
        if (path == "/api/pair") {
            cJSON_Delete(j);
            return requestPair() ? ok(req, "Pairing started")
                                 : error(req, "409 Conflict",
                                         "Pair unavailable in SNIFFER/mock, or inverter serial missing");
        }
        cJSON_Delete(j);
        return error(req, "404 Not Found", "Unknown endpoint");
    }
    if (path == "/api/status" || path == "/api/system")
        return json(req, systemJson());
    if (path == "/api/live")
        return json(req, liveJson());
    if (path == "/api/config")
        return json(req, configuration());
    if (path == "/api/wifi")
        return json(req, wifiJson());
    if (path == "/api/wifi/scan")
        return json(req, wifiScan());
    if (path == "/api/stats") {
        auto serial = query(req, "serial", configGet().serial);
        if (serial == "all")
            return json(req, statsAllJson());
        uint8_t id[6];
        if (!serial.empty() && !parseHex(serial.c_str(), id, 6))
            return error(req, "400 Bad Request", "Invalid serial");
        return json(req, statsJson(serial.c_str()));
    }
    if (path == "/api/history")
        return history(req);
    if (path == "/api/export.csv")
        return csv(req);
    if (path == "/api/backup")
        return backup(req);
    if (path == "/api/debug")
        return json(req, logJson());
    if (path == "/api/events")
        return events(req, false);
    if (path == "/api/sniffer/events")
        return events(req, true);
    if (path == "/api/sniffer/export")
        return captureExport(req);
    if (path == "/api/sniffer") {
        uint64_t after = 0;
        if (!integer(query(req, "after", "0"), after))
            return error(req, "400 Bad Request", "after must be an integer");
        return json(req, snifferJson(after), 16384);
    }
    return error(req, "404 Not Found", "Unknown endpoint");
}
void apiRegister(httpd_handle_t server) {
    httpd_uri_t get{};
    get.uri = "/api/*";
    get.method = HTTP_GET;
    get.handler = handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
    httpd_uri_t post = get;
    post.method = HTTP_POST;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
}
} // namespace sol
