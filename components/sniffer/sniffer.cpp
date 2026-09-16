#include "sniffer.hpp"
#include "config.hpp"
#include "esp_timer.h"
#include "fleet.hpp"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "logging.hpp"
#include <algorithm>
#include <atomic>
#include <sys/time.h>
namespace sol {
static constexpr size_t bufferSize = 256;
static std::array<CapturedFrame, bufferSize> frames;
static uint64_t sequence = 0;
static size_t count = 0;
static SemaphoreHandle_t mutex;
static QueueHandle_t controls;
static std::atomic<bool> active{false};
static bool paused = false, scanning = false;
static uint8_t currentChannel = 16;
static uint32_t dwell = 5000;
static uint64_t scanStart = 0, stopAt = 0, captureStart = 0, listeningUs = 0, allFrames = 0, apsFrames = 0;
template <typename T, size_t N> struct Set {
    std::array<T, N> values{};
    size_t count = 0;
    bool overflow = false;
    void add(T v) {
        for (size_t i = 0; i < count; i++)
            if (values[i] == v)
                return;
        if (count < N)
            values[count++] = v;
        else
            overflow = true;
    }
};
struct Channel {
    uint32_t frames = 0, aps = 0;
    int64_t sum = 0;
    int min = 127, max = -128;
    uint64_t listeningMs = 0;
    Set<uint64_t, 24> src, dst;
    Set<uint16_t, 16> pans;
};
static std::array<Channel, 16> channels;
// RSSI/LQI belong to the physical transmitter and live in `radio` (RadioObservability) below,
// never on this logical/NWK-keyed table.
struct Device {
    bool used = false;
    uint16_t pan = 0, address = 0;
    uint32_t frames = 0, requests = 0, responses = 0, matched = 0, intervals = 0;
    uint64_t lastRequest = 0, intervalUs = 0, responseUs = 0;
    unsigned requestSize = 0, responseSize = 0;
    char serial[13] = {};
    bool ecu = false, inverter = false, awaitingResponse = false;
};
static std::array<Device, 24> devices;
static uint32_t deviceOverflow = 0;
static APSReassembler classifier;
// Named distinctly from the ESP32C6Radio& parameter used elsewhere in this file (snifferRun).
static RadioObservability observability;
// Scratch buffer for the classifier's completed payload. Kept static rather than a local in
// ingest(): at ~620 bytes it is too large to add to that function's stack frame on the
// "radio" task (6144-byte stack, shared with the 802.15.4 driver's own call depth). ingest()
// runs strictly sequentially on that single task, so reuse across calls is safe.
static APSPayload fragmentPayload;
struct Control {
    uint8_t action = 0, channel = 16;
    uint32_t duration = 0, dwell = 5000;
};
static Device *device(uint16_t pan, uint16_t addr) {
    for (auto &d : devices)
        if (d.used && d.pan == pan && d.address == addr)
            return &d;
    for (auto &d : devices)
        if (!d.used) {
            d.used = true;
            d.pan = pan;
            d.address = addr;
            return &d;
        }
    ++deviceOverflow;
    return nullptr;
}
static void onFragmentTimeout(uint16_t pan, uint16_t nwkSrc) {
    observability.noteTimeout(pan, nwkSrc);
}
bool snifferBegin() {
    mutex = xSemaphoreCreateMutex();
    controls = xQueueCreate(4, sizeof(Control));
    classifier.onTimeout = onFragmentTimeout;
    return mutex && controls;
}
bool snifferActive() {
    return active;
}
bool snifferControl(const cJSON *j) {
    if (!active || !cJSON_IsObject(j))
        return false;
    auto a = cJSON_GetObjectItemCaseSensitive(j, "action");
    if (!cJSON_IsString(a))
        return false;
    Control c;
    if (!strcmp(a->valuestring, "pause"))
        c.action = 1;
    else if (!strcmp(a->valuestring, "resume"))
        c.action = 2;
    else if (!strcmp(a->valuestring, "clear"))
        c.action = 3;
    else if (!strcmp(a->valuestring, "scan"))
        c.action = 4;
    else if (!strcmp(a->valuestring, "capture"))
        c.action = 5;
    else if (!strcmp(a->valuestring, "channel"))
        c.action = 6;
    else
        return false;
    auto ch = cJSON_GetObjectItemCaseSensitive(j, "channel");
    if (ch) {
        if (!cJSON_IsNumber(ch) || ch->valuedouble < 11 || ch->valuedouble > 26 ||
            floor(ch->valuedouble) != ch->valuedouble)
            return false;
        c.channel = ch->valueint;
    }
    auto d = cJSON_GetObjectItemCaseSensitive(j, "duration");
    if (d) {
        if (!cJSON_IsNumber(d) || d->valueint < 1 || d->valueint > 600)
            return false;
        c.duration = d->valueint;
    }
    auto dw = cJSON_GetObjectItemCaseSensitive(j, "dwell");
    if (dw) {
        if (!cJSON_IsNumber(dw) || dw->valueint < 3 || dw->valueint > 10)
            return false;
        c.dwell = dw->valueint * 1000;
    }
    return xQueueSend(controls, &c, 0) == pdTRUE;
}
static void ingest(const ESP32C6Radio::Packet &p) {
    CapturedFrame f;
    f.monotonicUs = p.monotonicUs;
    f.channel = p.channel;
    f.rssi = p.rssi;
    f.lqi = p.lqi;
    f.length = p.length;
    memcpy(f.bytes.data(), p.bytes, p.length);
    timeval wall{};
    gettimeofday(&wall, nullptr);
    if (wall.tv_sec >= 1704067200)
        f.epochUs = uint64_t(wall.tv_sec) * 1000000 + wall.tv_usec - (esp_timer_get_time() - p.monotonicUs);
    auto parsed = parseFrame(p.bytes, p.length);
    if (parsed.apsValid && parsed.profile == 0x0f05 && parsed.cluster == 0x0106)
        fleetCapture(f);
    // Reassembly-aware classification: a lone fragment must never be displayed/counted as a
    // resolved APsystems message type. `classifier` covers all APS traffic (not just cluster
    // 0x0106) purely for display/statistics; it never feeds the inverter decoder (fleet.cpp owns
    // its own independent instance for that).
    f.resolvedClassification = parsed.classification;
    strcpy(f.resolvedSerial, parsed.serial);
    auto outcome = APSReassembler::Outcome::Conflict; // sentinel: "not a complete APS message"
    bool fragmented = false;
    if (parsed.apsValid) {
        if (!parsed.fragmentation) {
            outcome = APSReassembler::Outcome::Ready;
        } else {
            fragmented = true;
            outcome = classifier.add(f, fragmentPayload);
            switch (outcome) {
            case APSReassembler::Outcome::Ready: {
                auto resolved = classifyReassembled(fragmentPayload.header, fragmentPayload.bytes.data(),
                                                    fragmentPayload.length);
                f.resolvedClassification = resolved.classification;
                strcpy(f.resolvedSerial, resolved.serial);
                break;
            }
            case APSReassembler::Outcome::Duplicate:
                f.resolvedClassification = Classification::ApsFragmentDuplicate;
                break;
            case APSReassembler::Outcome::Orphan:
            case APSReassembler::Outcome::Conflict:
                f.resolvedClassification = Classification::ApsFragmentOrphan;
                break;
            case APSReassembler::Outcome::Pending:
                break; // already ApsFragmentPending from parseFrame.
            }
        }
    }
    observability.observe(parsed, p.monotonicUs, p.channel, p.rssi, p.lqi, outcome, fragmented);
    Guard g(mutex);
    f.id = ++sequence;
    frames[(sequence - 1) % bufferSize] = f;
    if (count < bufferSize)
        ++count;
    ++allFrames;
    bool aps = parsed.classification >= Classification::APSUnknown;
    if (aps)
        ++apsFrames;
    if (p.channel < 11 || p.channel > 26)
        return;
    auto &ch = channels[p.channel - 11];
    ++ch.frames;
    if (aps)
        ++ch.aps;
    ch.sum += p.rssi;
    ch.min = std::min(ch.min, int(p.rssi));
    ch.max = std::max(ch.max, int(p.rssi));
    if (parsed.valid && parsed.srcMode)
        ch.src.add(parsed.src);
    if (parsed.valid && parsed.dstMode)
        ch.dst.add(parsed.dst);
    if (parsed.srcPanValid)
        ch.pans.add(parsed.srcPan);
    if (parsed.dstPanValid)
        ch.pans.add(parsed.dstPan);
    if (!parsed.nwkValid)
        return;
    auto d = device(parsed.srcPan, parsed.nwkSrc);
    if (d)
        ++d->frames;
    if (f.resolvedClassification == Classification::Poll) {
        if (d)
            d->ecu = true;
        auto target = device(parsed.srcPan, parsed.nwkDst);
        if (target) {
            if (target->lastRequest) {
                target->intervalUs += p.monotonicUs - target->lastRequest;
                ++target->intervals;
            }
            target->lastRequest = p.monotonicUs;
            target->awaitingResponse = true;
            target->requestSize = p.length;
            ++target->requests;
        }
    }
    if (f.resolvedClassification == Classification::Response && d) {
        d->inverter = true;
        strcpy(d->serial, f.resolvedSerial);
        ++d->responses;
        d->responseSize = p.length;
        if (d->awaitingResponse && d->lastRequest && p.monotonicUs > d->lastRequest &&
            p.monotonicUs - d->lastRequest < 10000000) {
            d->responseUs += p.monotonicUs - d->lastRequest;
            ++d->matched;
            d->awaitingResponse = false;
        }
    }
}
void snifferRun(ESP32C6Radio &radio) {
    active = true;
    currentChannel = radio.channel;
    captureStart = esp_timer_get_time();
    uint64_t lastTick = captureStart;
    log(2, "SNIFFER: Passive mode - no radio transmission; automatic ACK disabled by promiscuous driver");
    for (;;) {
        Control cmd;
        uint64_t now = esp_timer_get_time();
        {
            Guard g(mutex);
            if (!paused) {
                channels[currentChannel - 11].listeningMs += (now - lastTick) / 1000;
                listeningUs += now - lastTick;
            }
            lastTick = now;
        }
        if (xQueueReceive(controls, &cmd, 0) == pdTRUE) {
            bool tune = false;
            {
                Guard g(mutex);
                if (cmd.action == 1) {
                    paused = true;
                    scanning = false;
                }
                if (cmd.action == 2) {
                    paused = false;
                    stopAt = 0;
                }
                if (cmd.action == 3 || cmd.action == 4 || cmd.action == 5) {
                    count = 0;
                    allFrames = apsFrames = 0;
                    channels = {};
                    devices = {};
                    deviceOverflow = 0;
                    classifier = {};
                    classifier.onTimeout = onFragmentTimeout;
                    observability = {};
                    captureStart = now;
                    listeningUs = 0;
                }
                if (cmd.action == 4) {
                    channels = {};
                    scanning = true;
                    paused = false;
                    currentChannel = 11;
                    dwell = cmd.dwell;
                    scanStart = now;
                    stopAt = 0;
                    tune = true;
                }
                if (cmd.action == 5 || cmd.action == 6) {
                    scanning = false;
                    paused = false;
                    currentChannel = cmd.channel;
                    captureStart = now;
                    stopAt = cmd.duration ? now + uint64_t(cmd.duration) * 1000000 : 0;
                    tune = true;
                }
            }
            if (tune && !radio.tune(currentChannel))
                log(0, "Sniffer channel switch failed");
        }
        bool tune = false;
        {
            Guard g(mutex);
            if (stopAt && now >= stopAt) {
                paused = true;
                stopAt = 0;
                log(2, "Capture completed; RAM buffer ready to export");
            }
            if (scanning && now - scanStart >= uint64_t(dwell) * 1000) {
                if (currentChannel < 26) {
                    ++currentChannel;
                    scanStart = now;
                    tune = true;
                } else {
                    scanning = false;
                    paused = true;
                }
            }
        }
        if (tune)
            radio.tune(currentChannel);
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ESP32C6Radio::Packet p;
        if (radio.receivePacket(p, 20))
            ingest(p);
    }
}
static void number(cJSON *j, const char *k, double v) {
    if (std::isfinite(v))
        cJSON_AddNumberToObject(j, k, v);
    else
        cJSON_AddNullToObject(j, k);
}
static void addr(cJSON *j, const char *key, uint64_t value, unsigned mode) {
    if (!mode) {
        cJSON_AddNullToObject(j, key);
        return;
    }
    char b[20];
    snprintf(b, sizeof(b), mode == 2 ? "%04llX" : "%016llX", value);
    cJSON_AddStringToObject(j, key, b);
}
cJSON *captureJson(const CapturedFrame &f) {
    auto p = parseFrame(f.bytes.data(), f.length);
    auto j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", f.id);
    cJSON_AddNumberToObject(j, "timestampUs", f.epochUs);
    cJSON_AddNumberToObject(j, "monotonicUs", f.monotonicUs);
    cJSON_AddNumberToObject(j, "channel", f.channel);
    number(j, "rssi", f.rssi);
    number(j, "lqi", f.lqi);
    number(j, "length", f.length);
    // Persisted at ingest time by the reassembly-aware classifier, not recomputed here: a
    // lone fragment's raw single-frame classification must never resolve to a known
    // APsystems message type (see f.resolvedClassification / classifyReassembled).
    cJSON_AddStringToObject(j, "protocol", classificationName(f.resolvedClassification));
    if (f.resolvedSerial[0])
        cJSON_AddStringToObject(j, "resolvedSerial", f.resolvedSerial);
    cJSON_AddStringToObject(j, "parseState",
                            p.encrypted  ? "encrypted"
                            : p.unparsed ? "unparsed"
                            : p.valid    ? "parsed"
                                         : "unknown");
    number(j, "frameControl", p.control);
    number(j, "type", p.type);
    number(j, "version", p.version);
    number(j, "srcMode", p.srcMode);
    number(j, "dstMode", p.dstMode);
    number(j, "sequence", p.sequenceValid ? p.sequence : missing);
    cJSON_AddBoolToObject(j, "ackRequest", p.control & 0x20);
    cJSON_AddBoolToObject(j, "securityEnabled", p.control & 8);
    cJSON_AddBoolToObject(j, "framePending", p.control & 0x10);
    cJSON_AddBoolToObject(j, "panCompression", p.control & 0x40);
    number(j, "srcPan", p.srcPanValid ? p.srcPan : missing);
    number(j, "dstPan", p.dstPanValid ? p.dstPan : missing);
    addr(j, "src", p.src, p.valid ? p.srcMode : 0);
    addr(j, "dst", p.dst, p.valid ? p.dstMode : 0);
    number(j, "nwkSrc", p.nwkValid ? p.nwkSrc : missing);
    number(j, "nwkDst", p.nwkValid ? p.nwkDst : missing);
    number(j, "profile", p.apsValid ? p.profile : missing);
    number(j, "cluster", p.apsValid ? p.cluster : missing);
    number(j, "sourceEndpoint", p.apsValid ? p.srcEp : missing);
    number(j, "destinationEndpoint", p.apsValid ? p.dstEp : missing);
    number(j, "apsCounter", p.apsValid ? p.apsCounter : missing);
    number(j, "fragmentation", p.apsValid ? p.fragmentation : missing);
    number(j, "fragmentBlock", p.apsValid ? p.fragmentBlock : missing);
    number(j, "apsType", p.apsValid ? p.apsType : missing);
    number(j, "payloadOffset", p.payloadOffset);
    number(j, "payloadLength", p.payloadLength);
    char hex[251];
    for (size_t i = 0; i < f.length; i++)
        snprintf(hex + 2 * i, 3, "%02X", f.bytes[i]);
    hex[f.length * 2] = 0;
    cJSON_AddStringToObject(j, "hex", hex);
    return j;
}
cJSON *snifferJson(uint64_t after, size_t limit) {
    auto j = cJSON_CreateObject();
    auto batch = cJSON_AddArrayToObject(j, "frames");
    Guard g(mutex);
    cJSON_AddBoolToObject(j, "active", active);
    cJSON_AddBoolToObject(j, "rxOnly", active);
    cJSON_AddBoolToObject(j, "paused", paused);
    cJSON_AddBoolToObject(j, "scanning", scanning);
    number(j, "channel", currentChannel);
    number(j, "totalFrames", allFrames);
    number(j, "apsFrames", apsFrames);
    number(j, "unknownFrames", allFrames - apsFrames);
    number(j, "bufferCapacity", bufferSize);
    number(j, "bufferCount", count);
    number(j, "cursor", sequence);
    number(j, "overwritten", allFrames > count ? allFrames - count : 0);
    number(j, "durationSeconds", listeningUs / 1000000.0);
    auto cs = cJSON_AddArrayToObject(j, "channels");
    int best = -1;
    uint32_t score = 0;
    for (unsigned i = 0; i < 16; i++) {
        auto &ch = channels[i];
        auto v = cJSON_CreateObject();
        number(v, "channel", i + 11);
        number(v, "frames", ch.frames);
        number(v, "apsFrames", ch.aps);
        number(v, "framesPerSecond", ch.listeningMs ? ch.frames * 1000.0 / ch.listeningMs : 0);
        number(v, "averageRssi", ch.frames ? double(ch.sum) / ch.frames : missing);
        number(v, "maxRssi", ch.frames ? ch.max : missing);
        number(v, "uniqueSources", ch.src.count);
        number(v, "uniqueDestinations", ch.dst.count);
        auto pans = cJSON_AddArrayToObject(v, "pans");
        for (size_t k = 0; k < ch.pans.count; k++)
            cJSON_AddItemToArray(pans, cJSON_CreateNumber(ch.pans.values[k]));
        cJSON_AddBoolToObject(v, "addressCountsTruncated",
                              ch.src.overflow || ch.dst.overflow || ch.pans.overflow);
        cJSON_AddItemToArray(cs, v);
        if (ch.aps > score) {
            score = ch.aps;
            best = i;
        }
    }
    number(j, "probableChannel", best >= 0 ? best + 11 : missing);
    cJSON_AddStringToObject(j, "confidence",
                            score >= 5 ? "INFERRED: repeated matching profile/endpoint"
                            : score    ? "INFERRED: few matching frames"
                                       : "UNKNOWN");
    auto findLogical = [&](uint16_t pan, uint16_t address) -> const RadioObservability::LogicalSource * {
        for (auto &l : observability.logical())
            if (l.used && l.pan == pan && l.address == address)
                return &l;
        return nullptr;
    };
    auto findPhysical = [&](uint64_t mac) -> const RadioObservability::PhysicalTransmitter * {
        for (auto &t : observability.physical())
            if (t.used && t.mac == mac)
                return &t;
        return nullptr;
    };
    auto ds = cJSON_AddArrayToObject(j, "devices");
    for (auto &d : devices)
        if (d.used) {
            auto v = cJSON_CreateObject();
            number(v, "pan", d.pan);
            addr(v, "address", d.address, 2);
            cJSON_AddNullToObject(v, "ieeeAddress");
            cJSON_AddStringToObject(v, "role",
                                    d.ecu        ? "ECU candidate"
                                    : d.inverter ? "APsystems inverter candidate"
                                                 : "UNKNOWN");
            cJSON_AddStringToObject(v, "confidence", d.ecu || d.inverter ? "INFERRED" : "DETECTED");
            cJSON_AddStringToObject(v, "possibleSerial", d.serial);
            number(v, "frames", d.frames);
            number(v, "requests", d.requests);
            number(v, "responses", d.responses);
            // RSSI is the physical transmitter's, never the logical/NWK identity's. Only shown
            // here for a direct hop (a physical transmitter whose mac equals this NWK address);
            // a device heard only via relay reports null rather than a blended value.
            auto direct = findPhysical(d.address);
            number(v, "averageRssi", direct ? double(direct->rssiSum) / direct->frames : missing);
            number(v, "minRssi", direct ? direct->rssiMin : missing);
            number(v, "maxRssi", direct ? direct->rssiMax : missing);
            number(v, "pollingIntervalMs", d.intervals ? double(d.intervalUs) / d.intervals / 1000 : missing);
            number(v, "responseDelayMs", d.matched ? double(d.responseUs) / d.matched / 1000 : missing);
            number(v, "requestSize", d.requestSize);
            number(v, "responseSize", d.responseSize);
            auto logical = findLogical(d.pan, d.address);
            number(v, "completeMessages", logical ? logical->completeMessages : 0);
            number(v, "fragmentedMessages", logical ? logical->fragmentedMessages : 0);
            number(v, "duplicateFragments", logical ? logical->duplicateFragments : 0);
            number(v, "orphanFragments", logical ? logical->orphanFragments : 0);
            number(v, "reassemblyTimeouts", logical ? logical->reassemblyTimeouts : 0);
            // Observed physical relays for this logical source. A count, never a claim of a
            // fixed/permanent route.
            auto relays = cJSON_AddArrayToObject(v, "relays");
            for (auto &r : observability.relays())
                if (r.used && r.nwkSrc == d.address) {
                    auto rv = cJSON_CreateObject();
                    addr(rv, "mac", r.mac, 2);
                    number(rv, "frames", r.frames);
                    cJSON_AddItemToArray(relays, rv);
                }
            cJSON_AddItemToArray(ds, v);
        }
    number(j, "deviceOverflow", deviceOverflow);
    auto phys = cJSON_AddArrayToObject(j, "physicalTransmitters");
    for (auto &t : observability.physical())
        if (t.used) {
            auto v = cJSON_CreateObject();
            addr(v, "mac", t.mac, 2);
            number(v, "channel", t.channel);
            number(v, "frames", t.frames);
            number(v, "averageRssi", double(t.rssiSum) / t.frames);
            number(v, "minRssi", t.rssiMin);
            number(v, "maxRssi", t.rssiMax);
            number(v, "averageLqi", double(t.lqiSum) / t.frames);
            cJSON_AddItemToArray(phys, v);
        }
    number(j, "physicalTransmitterOverflow", observability.physicalOverflow());
    number(j, "relayOverflow", observability.relayOverflow());
    number(j, "logicalSourceOverflow", observability.logicalOverflow());
    auto stats = classifier.stats();
    auto apsMessages = cJSON_AddObjectToObject(j, "apsMessages");
    number(apsMessages, "total", stats.total);
    number(apsMessages, "nonFragmented", stats.nonFragmented);
    number(apsMessages, "fragmented", stats.fragmented);
    number(apsMessages, "reassembled", stats.reassembled);
    number(apsMessages, "duplicateFragments", stats.duplicateFragments);
    number(apsMessages, "orphanFragments", stats.orphanFragments);
    number(apsMessages, "conflicts", stats.conflicts);
    number(apsMessages, "timeouts", stats.timeouts);
    size_t added = 0;
    for (uint64_t id = sequence - count + 1; id <= sequence && added < limit; id++)
        if (id > after) {
            cJSON_AddItemToArray(batch, captureJson(frames[(id - 1) % bufferSize]));
            ++added;
        }
    return j;
}
bool snifferVisit(const std::function<bool(const CapturedFrame &)> &fn) {
    uint64_t first, last;
    {
        Guard g(mutex);
        last = sequence;
        first = sequence - count + 1;
    }
    for (uint64_t id = first; id <= last; id++) {
        CapturedFrame f;
        {
            Guard g(mutex);
            f = frames[(id - 1) % bufferSize];
        }
        if (f.id == id && !fn(f))
            return false;
    }
    return true;
}
} // namespace sol
