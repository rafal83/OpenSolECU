#include "storage.hpp"
#include "esp_partition.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logging.hpp"
namespace sol {
class Flash final : public BlockDevice {
  public:
    const esp_partition_t *partition = nullptr;
    bool read(size_t o, void *d, size_t n) override {
        return esp_partition_read(partition, o, d, n) == ESP_OK;
    }
    bool write(size_t o, const void *d, size_t n) override {
        return esp_partition_write(partition, o, d, n) == ESP_OK;
    }
    bool erase(size_t o, size_t n) override {
        return esp_partition_erase_range(partition, o, n) == ESP_OK;
    }
};
static Flash flash;
// 414 sectors (1,695,744 bytes) of the 416-sector (0x1a0000) `storage` partition; the
// partition keeps a couple of sectors of slack over what's actually addressed here.
// Sized (at 64 bytes/record, 64 records/sector) for: ~2 days of per-minute detail (the dashboard's
// selected-day chart mainly needs today/yesterday), ~55 days of quarter-hour detail at 3
// configured inverters, ~43 days of per-inverter daily breakdown, and ~5 years of the
// all-inverters-combined daily total (dayAll) that outlives it. All four numbers scale with
// 1/inverterCount except dayAll, which is one record/day regardless of inverter count.
static constexpr size_t minuteSectors = 135, quarterSectors = 248, daySectors = 2, dayAllSectors = 29;
static Journal minute(flash, 0, minuteSectors), quarter(flash, minuteSectors * 4096, quarterSectors),
    day(flash, (minuteSectors + quarterSectors) * 4096, daySectors),
    dayAll(flash, (minuteSectors + quarterSectors + daySectors) * 4096, dayAllSectors);
static SemaphoreHandle_t mutex;
static QueueHandle_t queue;
static uint32_t failures = 0, dropped = 0;
static Journal &journal(Resolution r) {
    return r == Resolution::Minute   ? minute
          : r == Resolution::Quarter ? quarter
          : r == Resolution::Day     ? day
                                      : dayAll;
}
static void worker(void *) {
    Record r;
    for (;;)
        if (xQueueReceive(queue, &r, portMAX_DELAY) == pdTRUE) {
            bool ok;
            {
                Guard g(mutex);
                if (r.resolution == Resolution::Day || r.resolution == Resolution::DayAll) {
                    bool duplicate = false;
                    journal(r.resolution).visit([&](const Record &v) {
                        if (!strcmp(v.serial, r.serial) && v.day == r.day && v.totalWh >= r.totalWh)
                            duplicate = true;
                        return !duplicate;
                    });
                    if (duplicate)
                        continue;
                }
                ok = journal(r.resolution).append(r);
                if (!ok)
                    ++failures;
            }
            if (!ok)
                log(0, "Storage write failed; history point not persisted");
        }
}
bool storageBegin() {
    flash.partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               static_cast<esp_partition_subtype_t>(0x40), "storage");
    if (!flash.partition ||
        flash.partition->size < (minuteSectors + quarterSectors + daySectors + dayAllSectors) * 4096)
        return false;
    mutex = xSemaphoreCreateMutex();
    queue = xQueueCreate(12, sizeof(Record));
    if (!mutex || !queue || !minute.recover() || !quarter.recover() || !day.recover() || !dayAll.recover())
        return false;
    return xTaskCreate(worker, "storage", 4096, nullptr, 3, nullptr) == pdPASS;
}
bool storageRestore(Record &r) {
    Guard g(mutex);
    return minute.latest(r);
}
bool storageLatest(Resolution res, Record &r) {
    Guard g(mutex);
    return journal(res).latest(r);
}
bool storageEnqueue(const Record &r) {
    if (xQueueSend(queue, &r, pdMS_TO_TICKS(20)) == pdTRUE)
        return true;
    ++dropped;
    log(0, "Storage queue full; point dropped");
    return false;
}
bool storageVisit(Resolution r, const std::function<bool(const Record &)> &fn) {
    size_t start, capacity;
    uint64_t last;
    {
        Guard g(mutex);
        auto &j = journal(r);
        start = j.nextSlot();
        capacity = j.capacity();
        last = j.lastSequence();
    }
    for (size_t i = 0; i < capacity; i++) {
        Record record;
        bool valid;
        {
            Guard g(mutex);
            if (!journal(r).readSlot((start + i) % capacity, record, valid))
                return false;
        }
        if (valid && record.sequence <= last && !fn(record))
            return false;
    }
    return true;
}
cJSON *storageJson() {
    Guard g(mutex);
    auto j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "bytes", flash.partition->size);
    cJSON_AddNumberToObject(j, "usedBytes", (minuteSectors + quarterSectors + daySectors + dayAllSectors) * 4096);
    cJSON_AddNumberToObject(j, "minuteRecords", minute.count());
    cJSON_AddNumberToObject(j, "quarterRecords", quarter.count());
    cJSON_AddNumberToObject(j, "dailyRecords", day.count());
    cJSON_AddNumberToObject(j, "dailyAllRecords", dayAll.count());
    cJSON_AddNumberToObject(j, "writeFailures", failures);
    cJSON_AddNumberToObject(j, "dropped", dropped);
    cJSON_AddNumberToObject(j, "format", 4);
    return j;
}
} // namespace sol
