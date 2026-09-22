#include "fleet.hpp"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logging.hpp"
#include "storage.hpp"
#include <atomic>
#include <cstdio>

namespace sol {
static constexpr uint32_t passiveMaximumGapMs = 2U * 60U * 60U * 1000U;
struct View {
    InverterConfig config;
    InverterState state;
    Totals totals;
    bool configured = false, passive = false, measured = false;
    uint32_t messages = 0;
};
struct Slot {
    View view;
    APSystemsDecoder decoder;
    Aggregator aggregator;
};
static std::array<Slot, maxInverters> slots;
static size_t count = 0;
static SemaphoreHandle_t mutex;
static QueueHandle_t queue;
static APSReassembler reassembler;
static std::atomic<uint32_t> dropped{0}, rejected{0};
struct Input {
    bool capture = false;
    char serial[13] = {};
    InverterState state;
    CapturedFrame frame;
};
// Only the worker mutates slots. HTTP readers take a small, bounded snapshot.
static Slot *find(const char *serial) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(slots[i].view.config.serial, serial))
            return &slots[i];
    return nullptr;
}
static void restore(Slot &slot) {
    Record latest;
    bool found = false;
    storageVisit(Resolution::Minute, [&](const Record &r) {
        if (!strcmp(r.serial, slot.view.config.serial) && (!found || r.sequence > latest.sequence)) {
            latest = r;
            found = true;
        }
        return true;
    });
    if (found) {
        slot.aggregator.restore(latest);
        slot.view.totals = slot.aggregator.totals();
        slot.view.measured = true;
    }
}
static void publish(Slot &slot, InverterState state, bool passive) {
    slot.aggregator.add(state, [&](Record r) {
        if (!r.coverage && r.energyWh == 0 && (r.resolution != Resolution::Day || !slot.view.measured))
            return;
        strcpy(r.serial, slot.view.config.serial);
        r.flags = state.simulated ? 1 : 0;
        storageEnqueue(r);
    });
    Guard g(mutex);
    if (!state.online)
        state.lastSeen = slot.view.state.lastSeen;
    slot.view.state = state;
    slot.view.passive = passive;
    slot.view.measured |= state.online && std::isfinite(state.totalPower);
    slot.view.totals = slot.aggregator.totals();
    if (state.online)
        ++slot.view.messages;
}
// Sums each configured inverter's per-day Day record into one combined DayAll record, for
// calendar days old enough (2+ days) that every inverter has certainly rolled its own day over by
// now. The per-inverter Day ring is intentionally short (see components/storage); DayAll outlives
// it by years so the combined total survives long after the per-inverter breakdown has been
// evicted. Idempotent: storage's own append-time dedup drops a day already recorded with an
// equal-or-lower total, so re-running this after a reboot is harmless.
static void consolidateOldDays() {
    static uint64_t lastCheckMs = 0;
    static uint64_t consolidatedThrough = 0; // epoch start of the last day folded into dayAll
    uint64_t nowMs = esp_timer_get_time() / 1000;
    if (nowMs - lastCheckMs < 600000) // throttle: at most once every 10 minutes
        return;
    lastCheckMs = nowMs;
    time_t now = time(nullptr);
    if (now < 1704067200)
        return;
    if (!consolidatedThrough) {
        Record latest;
        consolidatedThrough =
            storageLatest(Resolution::DayAll, latest) ? uint64_t(latest.timestamp) : uint64_t(dayStart(now)) - 86400;
    }
    uint64_t cutoff = uint64_t(dayStart(now)) - 2 * 86400;
    size_t n;
    {
        Guard g(mutex);
        n = count;
    }
    while (consolidatedThrough < cutoff) {
        uint64_t dayStartTs = consolidatedThrough + 86400;
        int32_t key = dayKey(time_t(dayStartTs));
        double totalWh = 0;
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            char serial[13];
            {
                Guard g(mutex);
                strcpy(serial, slots[i].view.config.serial);
            }
            storageVisit(Resolution::Day, [&](const Record &r) {
                if (!strcmp(r.serial, serial) && r.day == key) {
                    totalWh += r.energyWh;
                    any = true;
                    return false;
                }
                return true;
            });
        }
        if (any) {
            Record combined;
            combined.resolution = Resolution::DayAll;
            combined.day = key;
            combined.timestamp = uint32_t(dayStartTs);
            combined.energyWh = float(totalWh);
            combined.dayWh = float(totalWh);
            // totalWh (lifetime cumulative) left at 0, peak left unknown: no single combined
            // figure is computed for either, matching consolidatedDayAll()'s existing stance on
            // values that aren't well-defined once summed across independently-tracked inverters.
            combined.peak = missing;
            storageEnqueue(combined);
        }
        consolidatedThrough = dayStartTs;
    }
}
static void worker(void *) {
    Input input;
    APSPayload payload;
    for (;;) {
        if (xQueueReceive(queue, &input, pdMS_TO_TICKS(60000)) != pdTRUE) {
            consolidateOldDays();
            continue;
        }
        consolidateOldDays();
        if (!input.capture) {
            if (auto slot = find(input.serial))
                publish(*slot, input.state, false);
            continue;
        }
        if (reassembler.add(input.frame, payload) != APSReassembler::Outcome::Ready ||
            payload.header.cluster != 0x0106)
            continue;
        char serial[13];
        for (int i = 0; i < 6; i++)
            snprintf(serial + 2 * i, 3, "%02X", payload.bytes[i]);
        auto slot = find(serial);
        InverterState state;
        // Passive reception can miss complete fragmented replies for several ECU cycles.
        // Counter deltas remain valid over a longer interval; the resulting power is an
        // interval average and is exposed as such through powerIntervalSeconds.
        state.maximumGapMs = passiveMaximumGapMs;
        state.timestamp = input.frame.epochUs / 1000000;
        state.monotonicMs = input.frame.monotonicUs / 1000;
        state.rssi = input.frame.rssi;
        state.lqi = input.frame.lqi;
        state.signalValid = true;
        APSystemsDecoder candidate;
        auto &decoder = slot ? slot->decoder : candidate;
        if (!decoder.decode(payload.bytes.data(), payload.length, payload.bytes.data(), state)) {
            ++rejected;
            continue;
        }
        if (!slot) {
            // Discover only a complete APsystems response with a valid application envelope.
            if (count == maxInverters)
                continue;
            slot = &slots[count];
            strcpy(slot->view.config.serial, serial);
            slot->view.config.model = candidate.detectedModel();
            slot->decoder = candidate;
            restore(*slot);
            {
                Guard g(mutex);
                ++count;
            }
            log(2, "Passive APsystems inverter discovered: %s (%s)", serial,
                inverterModelName(candidate.detectedModel()));
        }
        {
            Guard g(mutex);
            slot->view.config.address = payload.header.nwkSrc;
        }
        publish(*slot, state, true);
    }
}
bool fleetBegin() {
    mutex = xSemaphoreCreateMutex();
    queue = xQueueCreate(12, sizeof(Input));
    if (!mutex || !queue)
        return false;
    auto inventory = configInventory();
    count = inventory.count;
    for (size_t i = 0; i < count; i++) {
        slots[i].view.config = inventory.entries[i];
        slots[i].decoder.setModel(inventory.entries[i].model);
        slots[i].view.configured = true;
        restore(slots[i]);
    }
    // components/acquisition's radioWorker measured a genuine stack-protection-fault overflow at
    // 6144-7168 bytes running the same APSReassembler::add() call chain this worker() also runs
    // for passive frames (reassembly/observability nests several ParsedFrame-sized locals on top
    // of the 802.15.4 driver's own stack usage) -- generous margin here for the same reason, now
    // further loaded by consolidateOldDays()'s own nested storageVisit lambdas.
    return xTaskCreate(worker, "inverters", 12288, nullptr, 4, nullptr) == pdPASS;
}
void fleetCapture(const CapturedFrame &frame) {
    if (!queue)
        return;
    Input input;
    input.capture = true;
    input.frame = frame;
    if (xQueueSend(queue, &input, 0) != pdTRUE)
        ++dropped;
}
void fleetPublish(const char *serial, const InverterState &state) {
    Input input;
    strncpy(input.serial, serial, 12);
    input.state = state;
    if (xQueueSend(queue, &input, pdMS_TO_TICKS(20)) != pdTRUE)
        ++dropped;
}
static View snapshot(size_t i) {
    Guard g(mutex);
    return slots[i].view;
}
static View snapshot(const char *serial) {
    Guard g(mutex);
    auto slot = find(serial);
    return slot ? slot->view : View{};
}
static InverterState fresh(const View &v) {
    auto s = v.state;
    uint64_t now = esp_timer_get_time() / 1000;
    if (s.online && (now < s.monotonicMs || now - s.monotonicMs > s.maximumGapMs)) {
        auto last = s.lastSeen;
        s = {};
        s.lastSeen = last;
    }
    return s;
}
InverterState fleetState(const char *serial) {
    return fresh(snapshot(serial));
}
Totals fleetTotals(const char *serial) {
    auto v = snapshot(serial);
    return v.measured ? v.totals : Totals{};
}
static void num(cJSON *j, const char *key, double value) {
    if (std::isfinite(value))
        cJSON_AddNumberToObject(j, key, value);
    else
        cJSON_AddNullToObject(j, key);
}
static cJSON *viewJson(const View &v) {
    auto s = fresh(v);
    auto model = s.model;
    if (model == InverterModel::Auto)
        model = v.config.model == InverterModel::Auto ? inferInverterModel(v.config.serial) : v.config.model;
    uint8_t channelCount = s.channelCount ? s.channelCount : inverterModelChannels(model);
    if (!channelCount)
        channelCount = 2;
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "serial", v.config.serial);
    cJSON_AddStringToObject(j, "name", v.config.name);
    cJSON_AddNumberToObject(j, "address", v.config.address);
    cJSON_AddStringToObject(j, "model", inverterModelName(model));
    cJSON_AddStringToObject(j, "configuredModel", inverterModelName(v.config.model));
    cJSON_AddNumberToObject(j, "channelCount", channelCount);
    cJSON_AddBoolToObject(j, "configured", v.configured);
    cJSON_AddBoolToObject(j, "online", s.online);
    cJSON_AddBoolToObject(j, "simulated", s.simulated);
    cJSON_AddStringToObject(j, "source", v.passive ? "passive" : "active");
    cJSON_AddStringToObject(j, "status",
                            s.online ? (std::isfinite(s.totalPower) ? "measured" : "awaiting_second_sample")
                                     : (v.messages ? "stale" : "no_data"));
    num(j, "timestamp", s.timestamp >= 1704067200 ? s.timestamp : 0);
    num(j, "last_seen", s.lastSeen >= 1704067200 ? s.lastSeen : 0);
    num(j, "messages", v.messages);
    num(j, "totalPower", s.totalPower);
    num(j, "powerIntervalSeconds", s.powerIntervalSeconds);
    num(j, "todayWh", v.measured && v.totals.day == dayKey(time(nullptr)) ? v.totals.dayWh : missing);
    num(j, "totalWh", v.measured ? v.totals.totalWh : missing);
    num(j, "acVoltage", s.acVoltage);
    num(j, "acFrequency", s.acFrequency);
    num(j, "temperature", s.temperature);
    num(j, "rssi", s.signalValid ? s.rssi : missing);
    num(j, "lqi", s.signalValid ? s.lqi : missing);
    auto channels = cJSON_AddArrayToObject(j, "channels");
    for (size_t i = 0; i < channelCount; i++) {
        auto p = cJSON_CreateObject();
        auto pv = s.channels[i];
        num(p, "power", pv.power);
        num(p, "voltage", pv.voltage);
        num(p, "current", pv.current);
        cJSON_AddItemToArray(channels, p);
    }
    // Temporary compatibility fields for existing API clients.
    for (size_t i = 0; i < 2; i++) {
        auto p = cJSON_AddObjectToObject(j, i ? "pv2" : "pv1");
        auto pv = s.channels[i];
        num(p, "power", pv.power);
        num(p, "voltage", pv.voltage);
        num(p, "current", pv.current);
    }
    return j;
}
cJSON *fleetJson() {
    size_t n;
    {
        Guard g(mutex);
        n = count;
    }
    auto root = viewJson(n ? snapshot(size_t(0)) : View{}); // Legacy fields identify the first inverter.
    auto list = cJSON_AddArrayToObject(root, "inverters");
    double power = 0, energy = 0;
    unsigned known = 0, knownEnergy = 0, online = 0, pvCount = 0;
    bool simulated = false;
    for (size_t i = 0; i < n; i++) {
        auto v = snapshot(i);
        auto s = fresh(v);
        cJSON_AddItemToArray(list, viewJson(v));
        if (std::isfinite(s.totalPower)) {
            power += s.totalPower;
            ++known;
        }
        if (v.measured && v.totals.day == dayKey(time(nullptr))) {
            energy += v.totals.dayWh;
            ++knownEnergy;
        }
        online += s.online;
        simulated |= s.simulated;
        auto model = s.model == InverterModel::Auto
                         ? (v.config.model == InverterModel::Auto ? inferInverterModel(v.config.serial)
                                                                  : v.config.model)
                         : s.model;
        pvCount += s.channelCount ? s.channelCount : std::max<uint8_t>(2, inverterModelChannels(model));
    }
    cJSON_DeleteItemFromObject(root, "totalPower");
    num(root, "totalPower", n && known == n ? power : missing);
    cJSON_DeleteItemFromObject(root, "todayWh");
    num(root, "todayWh", n && knownEnergy == n ? energy : missing);
    cJSON_DeleteItemFromObject(root, "simulated");
    cJSON_AddBoolToObject(root, "simulated", simulated);
    num(root, "measuredPower", known ? power : missing);
    num(root, "measuredTodayWh", knownEnergy ? energy : missing);
    num(root, "inverterCount", n);
    num(root, "pvCount", pvCount);
    num(root, "knownPowerCount", known);
    num(root, "onlineCount", online);
    num(root, "decodeDropped", dropped.load());
    num(root, "decodeRejected", rejected.load());
    return root;
}
} // namespace sol
