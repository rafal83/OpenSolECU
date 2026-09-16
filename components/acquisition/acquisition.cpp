#include "acquisition.hpp"
#include "config.hpp"
#include "esp_timer.h"
#include "fleet.hpp"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logging.hpp"
#include "protocol.hpp"
#include "radio.hpp"
#include "sniffer.hpp"
#include "storage.hpp"
#include <algorithm>
#include <atomic>
namespace sol {
static ESP32C6Radio radio;
static APSProtocol protocol;
static std::array<APSystemsDecoder, maxInverters> decoders;
static QueueHandle_t commands, results;
static std::atomic<bool> pairing{false};
static std::atomic<uint32_t> timeouts{0}, received{0}, rejected{0};
struct Command {
    bool pair;
    size_t index = 0;
    uint32_t request = 0;
};
struct Result {
    uint32_t request;
    InverterState state;
};
static void trace(const char *direction, const uint8_t *p, size_t n, int rssi, int lqi) {
    if (configGet().logLevel < 4)
        return;
    APSMessage m;
    APSProtocol::parse(p, n, m);
    char hex[251];
    for (size_t i = 0; i < std::min(n, size_t(125)); i++)
        snprintf(hex + 2 * i, 3, "%02X", p[i]);
    log(4, "%s ch=%u pan=%04X src=%04X dst=%04X ep=%02X cluster=%04X seq=%u RSSI=%d LQI=%d %s", direction,
        radio.channel, m.pan, m.source, m.destination, m.endpoint, m.cluster, m.sequence, rssi, lqi, hex);
}
static bool send(const Frame &f) {
    if (!f.length)
        return false;
    trace("TX APS", f.bytes.data(), f.length, 0, 0);
    return radio.send(f.bytes.data(), f.length);
}
static bool waitReply(uint32_t timeout, const Config &c, InverterState &s, APSystemsDecoder &decoder,
                      uint16_t *paired = nullptr) {
    uint64_t until = esp_timer_get_time() / 1000 + timeout;
    uint8_t serial[6];
    if (!parseHex(c.serial, serial, 6))
        return false;
    while (uint64_t(esp_timer_get_time() / 1000) < until) {
        uint8_t b[125];
        size_t n;
        if (!radio.receive(b, sizeof(b), &n, 50))
            continue;
        trace("RX APS", b, n, radio.rssi, radio.lqi);
        if (paired) {
            APSMessage m;
            if (!APSProtocol::parse(b, n, m) || m.networkCommand)
                continue;
            if (m.destination != 0 && m.destination != 0xffff)
                continue;
            if (m.cluster != 0x0101)
                continue;
            for (size_t i = 0; i + 6 <= m.length; i++)
                if (!memcmp(m.payload + i, serial, 6) && m.source && m.source < 0xfff8) {
                    *paired = m.source;
                    return true;
                }
            continue;
        }
        CapturedFrame frame;
        frame.epochUs = uint64_t(time(nullptr)) * 1000000;
        frame.monotonicUs = esp_timer_get_time();
        frame.channel = radio.channel;
        frame.rssi = radio.rssi;
        frame.lqi = radio.lqi;
        frame.length = std::min(n, frame.bytes.size());
        std::copy_n(b, frame.length, frame.bytes.begin());
        static APSReassembler reassembler;
        APSPayload payload;
        if (reassembler.add(frame, payload) != APSReassembler::Outcome::Ready)
            continue;
        if (payload.header.nwkSrc != c.inverterId || payload.header.srcPan != c.pan ||
            payload.header.cluster != 0x0106 ||
            (payload.header.nwkDst != 0 && payload.header.nwkDst != 0xffff))
            continue;
        s.timestamp = time(nullptr);
        s.maximumGapMs = std::max(c.pollSeconds * 1000, configInventory().count * 3500) + 5000;
        s.monotonicMs = esp_timer_get_time() / 1000;
        s.rssi = radio.rssi;
        s.lqi = radio.lqi;
        s.signalValid = true;
        if (decoder.decode(payload.bytes.data(), payload.length, serial, s)) {
            received++;
            return true;
        }
        rejected++;
    }
    return false;
}
static void radioWorker(void *) {
    auto c = configGet();
    protocol.pan = c.pan;
    parseHex(c.ecu, protocol.ecu.data(), 6);
    radio.pan = c.pan;
    radio.channel = c.channel;
    radio.extended[0] = radio.extended[1] = 0xff;
    std::reverse_copy(protocol.ecu.begin(), protocol.ecu.end(), radio.extended + 2);
    radio.passive = c.sniffer;
    if (!radio.begin()) {
        log(0, "Native 802.15.4 initialization failed");
        vTaskDelete(nullptr);
        return;
    }
    if (c.sniffer) {
        snifferRun(radio);
        vTaskDelete(nullptr);
        return;
    }
    send(protocol.routeRequest(0xfffc));
    send(protocol.normal());
    Command cmd;
    for (;;) {
        if (xQueueReceive(commands, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            uint8_t b[125];
            size_t n;
            while (radio.receive(b, sizeof(b), &n, 0))
                trace("RX APS", b, n, radio.rssi, radio.lqi);
            continue;
        }
        c = configGet();
        auto inventory = configInventory();
        if (cmd.index >= inventory.count)
            continue;
        strcpy(c.serial, inventory.entries[cmd.index].serial);
        c.inverterId = inventory.entries[cmd.index].address;
        auto &decoder = decoders[cmd.index];
        decoder.setModel(inventory.entries[cmd.index].model);
        if (c.sniffer) {
            pairing = false;
            continue;
        }
        if (cmd.pair) {
            uint8_t serial[6];
            uint16_t address = 0;
            if (parseHex(c.serial, serial, 6))
                for (unsigned step = 0; step < 4; step++) {
                    send(protocol.pair(step, serial));
                    InverterState discard;
                    waitReply(1500, c, discard, decoder, &address);
                }
            if (address && configSetInverterId(address))
                log(2, "Pair response received; inverter short address=%04X. Poll validation pending", address);
            else
                log(1, "Pairing timeout: no matching inverter response");
            decoder.reset();
            send(protocol.normal());
            pairing = false;
            continue;
        }
        InverterState s;
        bool ok = false;
        if (*c.serial && c.inverterId) {
            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                if (attempt)
                    send(protocol.routeRequest(c.inverterId));
                send(protocol.poll(c.inverterId));
                ok = waitReply(1100, c, s, decoder);
            }
        }
        if (!ok) {
            ++timeouts;
            s = {};
            s.timestamp = time(nullptr);
            s.monotonicMs = esp_timer_get_time() / 1000;
        }
        Result result{cmd.request, s};
        xQueueOverwrite(results, &result);
    }
}
static void acquisitionWorker(void *) {
    uint32_t request = 0;
    std::array<EnergyIntegrator, maxInverters> mockEnergy;
    TickType_t tick = xTaskGetTickCount();
    for (;;) {
        if (configGet().sniffer) {
            vTaskDelay(pdMS_TO_TICKS(500));
            tick = xTaskGetTickCount();
            continue;
        }
        auto inventory = configInventory();
        for (size_t index = 0; index < inventory.count; index++) {
            InverterState s;
            s.maximumGapMs = std::max(configGet().pollSeconds * 1000, inventory.count * 3500) + 5000;
            s.timestamp = time(nullptr);
            s.monotonicMs = esp_timer_get_time() / 1000;
#ifdef CONFIG_OPENSOLECU_MOCK_INVERTER
            s.simulated = true;
            s.model = inventory.entries[index].model == InverterModel::Auto
                          ? inferInverterModel(inventory.entries[index].serial)
                          : inventory.entries[index].model;
            s.channelCount = inverterModelChannels(s.model);
            if (!s.channelCount)
                s.channelCount = 2;
            s.online = (s.monotonicMs / 1000) % 300 < 280;
            if (s.online) {
                float wave = std::max(0.0, 0.65 + 0.3 * sin(s.monotonicMs / 180000.0));
                s.totalPower = 0;
                for (size_t channel = 0; channel < s.channelCount; channel++) {
                    float nominal = 380.0f - channel * 15.0f;
                    float voltage = 32.1f - channel * 0.3f;
                    s.channels[channel] = {nominal * wave, voltage, nominal * wave / voltage};
                    s.totalPower += s.channels[channel].power;
                }
                s.acVoltage = 230.2;
                s.acFrequency = 50;
                s.temperature = 24 + 20 * wave;
                s.lastSeen = s.timestamp;
            }
            s.energyDeltaWh = mockEnergy[index].add(s.totalPower, s.monotonicMs, s.online, s.maximumGapMs);
#else
            if (!pairing) {
                Command cmd{false, index, ++request};
                bool matched = false;
                if (xQueueSend(commands, &cmd, 0) == pdTRUE) {
                    uint64_t until = esp_timer_get_time() / 1000 + 4300;
                    Result result;
                    while (uint64_t(esp_timer_get_time() / 1000) < until) {
                        if (xQueueReceive(results, &result, pdMS_TO_TICKS(50)) == pdTRUE &&
                            result.request == cmd.request) {
                            s = result.state;
                            matched = true;
                            break;
                        }
                    }
                }
                if (!matched) {
                    s.online = false;
                    s.timestamp = time(nullptr);
                    s.monotonicMs = esp_timer_get_time() / 1000;
                }
            }
#endif
            fleetPublish(inventory.entries[index].serial, s);
        }
        vTaskDelayUntil(&tick, pdMS_TO_TICKS(configGet().pollSeconds * 1000));
    }
}
bool acquisitionBegin() {
    commands = xQueueCreate(2, sizeof(Command));
    results = xQueueCreate(1, sizeof(Result));
    if (!commands || !results)
        return false;
// snifferRun()/ingest() on this task measured a genuine stack-protection-fault overflow at
// both 6144 and 7168 bytes on real hardware (SP landed past the stack's lower bound each
// time). The reassembly/observability call chain added several nested ParsedFrame-sized
// locals on top of the 802.15.4 driver's own stack usage; this margin is generous on purpose
// rather than tuned to the byte, since the exact peak (driver ISR-adjacent usage included)
// isn't statically known.
#ifdef CONFIG_OPENSOLECU_MOCK_INVERTER
    if (configGet().sniffer && xTaskCreate(radioWorker, "radio", 12288, nullptr, 6, nullptr) != pdPASS)
        return false;
#else
    if (xTaskCreate(radioWorker, "radio", 12288, nullptr, 6, nullptr) != pdPASS)
        return false;
#endif
    return xTaskCreate(acquisitionWorker, "acquisition", 6144, nullptr, 4, nullptr) == pdPASS;
}
bool requestPair() {
    if (configGet().sniffer || snifferActive())
        return false;
#ifdef CONFIG_OPENSOLECU_MOCK_INVERTER
    return false;
#else
    bool expected = false;
    if (!*configGet().serial || !pairing.compare_exchange_strong(expected, true))
        return false;
    Command c{true};
    if (xQueueSend(commands, &c, 0) != pdTRUE) {
        pairing = false;
        return false;
    }
    return true;
#endif
}
bool passiveTxSelfTest() {
    if (!snifferActive())
        return false;
    const uint8_t frame[9] = {0x41, 0x88, 0, 0xff, 0xff, 0xff, 0xff, 0, 0};
    return !radio.send(frame, sizeof(frame));
}
InverterState liveState() {
    return fleetState(configGet().serial);
}
Totals liveTotals() {
    return fleetTotals(configGet().serial);
}
cJSON *liveJson() {
    return fleetJson();
}
cJSON *acquisitionJson() {
    auto j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "pairing", pairing);
    cJSON_AddNumberToObject(j, "received", received);
    cJSON_AddNumberToObject(j, "timeouts", timeouts);
    cJSON_AddNumberToObject(j, "rejected", rejected);
    cJSON_AddNumberToObject(j, "rxDropped", radio.dropped);
    cJSON_AddNumberToObject(j, "txSubmissions", radio.txSubmissions.load());
    cJSON_AddNumberToObject(j, "txBlocked", radio.txBlocked.load());
    cJSON_AddStringToObject(j, "protocol", "capture-derived APsystems DS3/YC600/QS1");
    return j;
}
} // namespace sol
