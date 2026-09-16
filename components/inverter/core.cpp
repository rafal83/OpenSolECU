#include "sol.hpp"
#include <algorithm>
#include <cstdio>

namespace sol {
const char *inverterModelName(InverterModel model) {
    switch (model) {
    case InverterModel::DS3:
        return "DS3";
    case InverterModel::YC600:
        return "YC600";
    case InverterModel::QS1:
        return "QS1";
    default:
        return "AUTO";
    }
}
bool parseInverterModel(const char *text, InverterModel &model) {
    for (auto candidate : {InverterModel::Auto, InverterModel::DS3, InverterModel::YC600,
                           InverterModel::QS1})
        if (text && !strcmp(text, inverterModelName(candidate))) {
            model = candidate;
            return true;
        }
    return false;
}
uint8_t inverterModelChannels(InverterModel model) {
    return model == InverterModel::QS1 ? 4 : model == InverterModel::Auto ? 0 : 2;
}
InverterModel inferInverterModel(const char *serial) {
    if (!serial || strlen(serial) != 12)
        return InverterModel::Auto;
    if (serial[0] == '8')
        return InverterModel::QS1;
    if (serial[0] == '7')
        return InverterModel::DS3;
    if (serial[0] == '4')
        return InverterModel::YC600;
    return InverterModel::Auto;
}
uint32_t crc32(const void *data, size_t n) {
    uint32_t crc = 0xffffffff;
    auto p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
bool parseHex(const char *s, uint8_t *dst, size_t n) {
    if (!s || strlen(s) != n * 2)
        return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; i++) {
        int a = digit(s[2 * i]), b = digit(s[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        dst[i] = (a << 4) | b;
    }
    return true;
}
int32_t dayKey(time_t t) {
    tm d{};
    localtime_r(&t, &d);
    return (d.tm_year + 1900) * 10000 + (d.tm_mon + 1) * 100 + d.tm_mday;
}
time_t dayStart(time_t t) {
    tm d{};
    localtime_r(&t, &d);
    d.tm_hour = d.tm_min = d.tm_sec = 0;
    d.tm_isdst = -1;
    return mktime(&d);
}
static uint16_t be16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | p[1];
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t(be16(p)) << 16) | be16(p + 2);
}
static uint32_t be24(const uint8_t *p) {
    return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}
static bool applicationEnvelope(const uint8_t *p, size_t n, const uint8_t serial[6], uint16_t &stored,
                                uint16_t &sum) {
    if (n < 13 || memcmp(p, serial, 6) || p[6] != 0xfb || p[7] != 0xfb || n != size_t(13 + p[8]) ||
        p[n - 2] != 0xfe || p[n - 1] != 0xfe)
        return false;
    sum = 0;
    for (size_t i = 8; i < n - 4; i++)
        sum += p[i];
    // YC600/QS1 reference captures use 0000 here; DS3 carries the actual additive checksum.
    stored = be16(p + n - 4);
    return true;
}
static void packedChannel(const uint8_t *p, size_t low, PVChannelState &channel) {
    uint16_t current = p[low] | (uint16_t(p[low + 1] & 0x0f) << 8);
    uint16_t voltage = (uint16_t(p[low + 1] >> 4)) | (uint16_t(p[low + 2]) << 4);
    channel.current = current * (27.5f / 4096.0f);
    channel.voltage = voltage * (82.5f / 4096.0f);
}
bool APSystemsDecoder::decode(const uint8_t *p, size_t n, const uint8_t serial[6], InverterState &s) {
    uint16_t storedChecksum = 0, checksum = 0;
    if (!applicationEnvelope(p, n, serial, storedChecksum, checksum))
        return false;
    InverterModel model = configured_;
    if (model == InverterModel::Auto) {
        if (p[8] == 0x5c)
            model = InverterModel::DS3;
        else if (p[8] == 0x51)
            // Known QS1 serials use the 8xx family; AUTO remains overrideable in settings.
            model = (serial[0] >> 4) == 8 ? InverterModel::QS1 : InverterModel::YC600;
        else
            return false;
    }
    if ((model == InverterModel::DS3 && (p[8] != 0x5c || n != 105 || p[9] != 0xbb || p[10] != 0xbb)) ||
        (model != InverterModel::DS3 && (p[8] != 0x51 || n != 94)))
        return false;
    if ((model == InverterModel::DS3 && storedChecksum != checksum) ||
        (model != InverterModel::DS3 && storedChecksum != 0 && storedChecksum != checksum))
        return false;
    s.model = model;
    s.channelCount = inverterModelChannels(model);
    if (model == InverterModel::DS3) {
        if (be16(p + 26) == 0xffff || be16(p + 28) == 0xffff || be32(p + 50) == 0xffffffff ||
            be32(p + 54) == 0xffffffff)
            return false;
        s.channels[0].voltage = be16(p + 26) / 48.0f;
        s.channels[1].voltage = be16(p + 28) / 48.0f;
        s.channels[0].current = be16(p + 30) * 0.0125f;
        s.channels[1].current = be16(p + 32) * 0.0125f;
        s.acVoltage = be16(p + 34) == 0xffff ? missing : be16(p + 34) / 3.8f;
        s.acFrequency = be16(p + 36) == 0xffff ? missing : be16(p + 36) / 100.0f;
        s.temperature = be16(p + 48) == 0xffff ? missing : be16(p + 48) * 0.0198f - 23.84f;
        s.rawEnergy[0] = be32(p + 50);
        s.rawEnergy[1] = be32(p + 54);
        s.inverterSeconds = be16(p + 38);
    } else {
        uint32_t frequencyPeriod = be24(p + 12);
        if (!frequencyPeriod)
            return false;
        s.temperature = be16(p + 10) * 0.2752f - 258.7f;
        s.acFrequency = 50000000.0f / frequencyPeriod;
        s.acVoltage = be16(p + 28) / 1.3277f / 4.0f;
        packedChannel(p, 25, s.channels[0]);
        packedChannel(p, 22, s.channels[1]);
        if (model == InverterModel::QS1) {
            packedChannel(p, 19, s.channels[2]);
            packedChannel(p, 16, s.channels[3]);
        }
        s.inverterSeconds = be16(p + (model == InverterModel::QS1 ? 30 : 17));
        for (size_t i = 0; i < s.channelCount; i++)
            s.rawEnergy[i] = be24(p + 37 + i * 5);
    }
    if (!std::isfinite(s.acVoltage) || s.acVoltage < 80 || s.acVoltage > 300 ||
        !std::isfinite(s.acFrequency) || s.acFrequency < 40 || s.acFrequency > 70 ||
        !std::isfinite(s.temperature) || s.temperature < -50 || s.temperature > 150)
        return false;
    for (size_t i = 0; i < s.channelCount; i++)
        if (!std::isfinite(s.channels[i].voltage) || s.channels[i].voltage < 0 ||
            s.channels[i].voltage > 100 || !std::isfinite(s.channels[i].current) ||
            s.channels[i].current < 0 || s.channels[i].current > 30)
            return false;
    if (detected_ != model)
        previous_ = false;
    detected_ = model;
    s.energyDeltaWh = 0;
    s.totalPower = missing;
    for (auto &channel : s.channels)
        channel.power = missing;
    uint16_t dt = uint16_t(s.inverterSeconds - seconds_);
    bool coherent = previous_ && s.monotonicMs > ms_ && s.monotonicMs - ms_ <= s.maximumGapMs && dt > 0 &&
                    dt <= s.maximumGapMs / 1000;
    for (size_t i = 0; i < s.channelCount; i++)
        coherent &= s.rawEnergy[i] >= energy_[i];
    if (coherent) {
        std::array<double, maxChannels> delta{};
        bool plausible = true;
        for (size_t i = 0; i < s.channelCount; i++) {
            double scale = model == InverterModel::DS3 ? 0.0000166 : 8.311 / 3600.0;
            delta[i] = (s.rawEnergy[i] - energy_[i]) * scale;
            plausible &= delta[i] * 3600 / dt <= 2000;
        }
        if (plausible) {
            s.totalPower = 0;
            for (size_t i = 0; i < s.channelCount; i++) {
                s.channels[i].power = delta[i] * 3600 / dt;
                s.totalPower += s.channels[i].power;
                s.energyDeltaWh += delta[i];
            }
        }
    }
    previous_ = true;
    seconds_ = s.inverterSeconds;
    ms_ = s.monotonicMs;
    energy_ = s.rawEnergy;
    s.countersValid = true;
    s.online = true;
    s.lastSeen = s.timestamp;
    return true;
}
double EnergyIntegrator::add(float p, uint64_t ms, bool online, uint32_t maxGap) {
    double wh = 0;
    if (online && std::isfinite(p) && p >= 0) {
        if (valid_ && ms > ms_ && ms - ms_ <= maxGap)
            wh = (power_ + p) * 0.5 * (ms - ms_) / 3600000.0;
        valid_ = true;
        power_ = p;
        ms_ = ms;
    } else
        valid_ = false;
    return wh;
}
static void put(uint8_t *p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[i] = v >> (i * 8);
}
static uint64_t get(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
static void putf(uint8_t *p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put(p, v, 4);
}
static float getf(const uint8_t *p) {
    uint32_t v = get(p, 4);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static void putPower(uint8_t *p, float power) {
    uint16_t encoded = 0xffff;
    if (std::isfinite(power) && power >= 0)
        encoded = uint16_t(std::min(65534.0, std::round(double(power) * 10.0)));
    put(p, encoded, 2);
}
static float getPower(const uint8_t *p) {
    auto encoded = uint16_t(get(p, 2));
    return encoded == 0xffff ? missing : encoded / 10.0f;
}
static void putd(uint8_t *p, double f) {
    uint64_t v;
    memcpy(&v, &f, 8);
    put(p, v, 8);
}
static double getd(const uint8_t *p) {
    uint64_t v = get(p, 8);
    double f;
    memcpy(&f, &v, 8);
    return f;
}
EncodedRecord encode(const Record &r) {
    EncodedRecord b{};
    memcpy(b.data(), "OSOL", 4);
    b[4] = 3;
    parseHex(r.serial, b.data() + 80, 6);
    b[5] = uint8_t(r.resolution);
    b[6] = r.flags;
    b[7] = std::min<uint8_t>(r.channelCount, maxChannels);
    put(b.data() + 8, r.sequence, 8);
    put(b.data() + 16, r.timestamp, 8);
    put(b.data() + 24, r.duration, 4);
    put(b.data() + 28, r.coverage, 4);
    put(b.data() + 32, r.day, 4);
    for (size_t i = 0; i < maxChannels; i++)
        putPower(b.data() + 36 + i * 2, r.channels[i]);
    putf(b.data() + 44, r.peak);
    put(b.data() + 48, r.peakTime, 8);
    putd(b.data() + 56, r.energyWh);
    putd(b.data() + 64, r.totalWh);
    putd(b.data() + 72, r.dayWh);
    put(b.data() + 92, crc32(b.data(), 92), 4);
    return b;
}
bool decode(const EncodedRecord &b, Record &r) {
    if (memcmp(b.data(), "OSOL", 4) || b[4] != 3 || b[5] < 1 || b[5] > 3 ||
        get(b.data() + 92, 4) != crc32(b.data(), 92))
        return false;
    r = {};
    for (int i = 0; i < 6; ++i)
        snprintf(r.serial + 2 * i, 3, "%02X", b[80 + i]);
    r.resolution = Resolution(b[5]);
    r.flags = b[6];
    r.sequence = get(b.data() + 8, 8);
    r.timestamp = get(b.data() + 16, 8);
    r.duration = get(b.data() + 24, 4);
    r.coverage = get(b.data() + 28, 4);
    r.day = get(b.data() + 32, 4);
    if (b[7] > maxChannels)
        return false;
    r.channelCount = b[7];
    for (size_t i = 0; i < maxChannels; i++)
        r.channels[i] = getPower(b.data() + 36 + i * 2);
    r.peak = getf(b.data() + 44);
    r.peakTime = get(b.data() + 48, 8);
    r.energyWh = getd(b.data() + 56);
    r.totalWh = getd(b.data() + 64);
    r.dayWh = getd(b.data() + 72);
    return std::isfinite(r.energyWh) && std::isfinite(r.totalWh) && std::isfinite(r.dayWh) &&
           r.energyWh >= 0 && r.totalWh >= 0 && r.dayWh >= 0;
}
bool Journal::recover() {
    sequence_ = 0;
    count_ = 0;
    size_t last = 0;
    EncodedRecord b;
    Record r;
    for (size_t i = 0; i < capacity(); i++) {
        if (!dev_.read(offset(i), b.data(), b.size()))
            return false;
        if (decode(b, r)) {
            ++count_;
            if (r.sequence > sequence_) {
                sequence_ = r.sequence;
                last = i;
            }
        }
    }
    next_ = sequence_ ? (last + 1) % capacity() : 0;
    // Skip torn, non-erased slots. Never rewrite partially programmed NOR flash.
    while (next_ % perSector) {
        if (!dev_.read(offset(next_), b.data(), b.size()))
            return false;
        if (std::all_of(b.begin(), b.end(), [](uint8_t v) { return v == 0xff; }))
            break;
        next_ = (next_ + 1) % capacity();
    }
    return true;
}
bool Journal::append(Record r) {
    if (next_ % perSector == 0) {
        size_t old = 0;
        EncodedRecord b;
        Record v;
        for (size_t i = 0; i < perSector; i++) {
            if (!dev_.read(offset(next_ + i), b.data(), b.size()))
                return false;
            if (decode(b, v))
                ++old;
        }
        if (!dev_.erase(offset(next_), 4096))
            return false;
        count_ -= std::min(count_, old);
    }
    r.sequence = sequence_ + 1;
    auto b = encode(r);
    if (!dev_.write(offset(next_), b.data(), b.size())) {
        recover();
        return false;
    }
    sequence_ = r.sequence;
    next_ = (next_ + 1) % capacity();
    ++count_;
    return true;
}
bool Journal::visit(const std::function<bool(const Record &)> &fn) {
    EncodedRecord b;
    Record r;
    for (size_t i = 0; i < capacity(); i++) {
        size_t slot = (next_ + i) % capacity();
        if (!dev_.read(offset(slot), b.data(), b.size()))
            return false;
        if (decode(b, r) && !fn(r))
            break;
    }
    return true;
}
bool Journal::latest(Record &r) {
    bool found = false;
    visit([&](const Record &v) {
        if (!found || v.sequence > r.sequence) {
            r = v;
            found = true;
        }
        return true;
    });
    return found;
}
bool Journal::readSlot(size_t slot, Record &record, bool &valid) {
    valid = false;
    if (slot >= capacity())
        return false;
    EncodedRecord bytes;
    if (!dev_.read(offset(slot), bytes.data(), bytes.size()))
        return false;
    valid = decode(bytes, record);
    return true;
}
void Aggregator::restore(const Record &r) {
    totals_ = {r.totalWh, r.dayWh, r.peak, r.peakTime, r.day};
}
void Aggregator::finish(Bucket &b, Resolution res, uint64_t end,
                        const std::function<void(const Record &)> &emit) {
    if (!b.start)
        return;
    Record r;
    r.timestamp = b.start;
    r.resolution = res;
    r.duration = uint32_t(end - b.start);
    r.coverage = b.coverage;
    r.day = totals_.day;
    r.channelCount = b.channelCount;
    for (size_t i = 0; i < b.channelCount; i++)
        r.channels[i] = b.coverage ? b.power[i] / b.coverage : missing;
    r.energyWh = b.wh;
    r.totalWh = totals_.totalWh;
    r.dayWh = totals_.dayWh;
    r.peak = totals_.peak;
    r.peakTime = totals_.peakTime;
    emit(r);
    b = {};
}
void Aggregator::add(const InverterState &s, const std::function<void(const Record &)> &emit) {
    double wh = s.online && std::isfinite(s.energyDeltaWh) && s.energyDeltaWh >= 0 ? s.energyDeltaWh : 0;
    if (s.timestamp < 1704067200) {
        totals_.totalWh += wh;
        previousMs_ = s.monotonicMs;
        return;
    }
    bool continuous = lastEpoch_ && s.timestamp > lastEpoch_ &&
                      s.timestamp - lastEpoch_ <= s.maximumGapMs / 1000 && s.monotonicMs > previousMs_ &&
                      s.monotonicMs - previousMs_ <= s.maximumGapMs;
    if (lastEpoch_ && s.timestamp < lastEpoch_) {
        minute_ = {};
        quarter_ = {};
    }
    auto advance = [&](uint64_t at) {
        int32_t day = dayKey(at);
        if (totals_.day && totals_.day != day) {
            finish(minute_, Resolution::Minute, minute_.start + 60, emit);
            finish(quarter_, Resolution::Quarter, quarter_.start + 900, emit);
            Record r;
            r.resolution = Resolution::Day;
            r.day = totals_.day;
            // Preserve the actual previous date even after multi-day outages.
            tm d{};
            d.tm_year = totals_.day / 10000 - 1900;
            d.tm_mon = (totals_.day / 100) % 100 - 1;
            d.tm_mday = totals_.day % 100;
            d.tm_isdst = -1;
            r.timestamp = mktime(&d);
            ++d.tm_mday;
            d.tm_isdst = -1;
            r.duration = mktime(&d) - r.timestamp;
            r.energyWh = totals_.dayWh;
            r.totalWh = totals_.totalWh;
            r.dayWh = totals_.dayWh;
            r.peak = totals_.peak;
            r.peakTime = totals_.peakTime;
            emit(r);
            totals_.dayWh = 0;
            totals_.peak = 0;
            totals_.peakTime = 0;
        }
        totals_.day = day;
        if (minute_.start && minute_.start / 60 != at / 60)
            finish(minute_, Resolution::Minute, minute_.start + 60, emit);
        if (quarter_.start && quarter_.start / 900 != at / 900)
            finish(quarter_, Resolution::Quarter, quarter_.start + 900, emit);
        if (!minute_.start)
            minute_.start = at / 60 * 60;
        if (!quarter_.start)
            quarter_.start = at / 900 * 900;
    };
    uint64_t start = continuous ? lastEpoch_ : s.timestamp;
    if (!continuous) {
        advance(start);
        totals_.totalWh += wh;
        totals_.dayWh += wh;
        minute_.wh += wh;
        quarter_.wh += wh;
    }
    while (start < s.timestamp) {
        advance(start);
        uint64_t end = std::min(s.timestamp, (start / 60 + 1) * 60);
        uint32_t seconds = end - start;
        double part = wh * double(seconds) / double(s.timestamp - lastEpoch_);
        totals_.totalWh += part;
        totals_.dayWh += part;
        if (s.online && std::isfinite(s.totalPower) && s.totalPower > totals_.peak) {
            totals_.peak = s.totalPower;
            totals_.peakTime = end - 1;
        }
        for (auto b : {&minute_, &quarter_}) {
            b->wh += part;
            bool complete = s.online && s.channelCount > 0 && s.channelCount <= maxChannels;
            for (size_t i = 0; i < s.channelCount && complete; i++)
                complete = std::isfinite(s.channels[i].power);
            if (complete) {
                b->channelCount = std::max(b->channelCount, s.channelCount);
                for (size_t i = 0; i < s.channelCount; i++)
                    b->power[i] += s.channels[i].power * seconds;
                b->coverage += seconds;
            }
        }
        start = end;
    }
    advance(s.timestamp);
    lastEpoch_ = s.timestamp;
    previousMs_ = s.monotonicMs;
}
} // namespace sol
