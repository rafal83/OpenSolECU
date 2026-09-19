#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#ifdef _WIN32
inline tm *localtime_r(const time_t *t, tm *result) {
    return localtime_s(result, t) == 0 ? result : nullptr;
}
#endif

namespace sol {
constexpr float missing = std::numeric_limits<float>::quiet_NaN();
constexpr size_t maxChannels = 4;
enum class InverterModel : uint8_t { Auto = 0, DS3 = 1, YC600 = 2, QS1 = 3 };
const char *inverterModelName(InverterModel model);
bool parseInverterModel(const char *text, InverterModel &model);
uint8_t inverterModelChannels(InverterModel model);
InverterModel inferInverterModel(const char *serial);
struct PVChannelState {
    float power = missing, voltage = missing, current = missing;
};
struct InverterState {
    uint32_t maximumGapMs = 15000;
    uint64_t timestamp = 0, lastSeen = 0, monotonicMs = 0;
    bool online = false, simulated = false;
    InverterModel model = InverterModel::Auto;
    uint8_t channelCount = 0;
    std::array<PVChannelState, maxChannels> channels{};
    float totalPower = missing, acVoltage = missing, acFrequency = missing, temperature = missing;
    int rssi = 0, lqi = 0;
    bool signalValid = false, countersValid = false;
    std::array<uint32_t, maxChannels> rawEnergy{};
    uint16_t inverterSeconds = 0;
    uint32_t powerIntervalSeconds = 0;
    double energyDeltaWh = 0;
};
template <typename T, size_t N> class Ring {
    std::array<T, N> data_{};
    size_t head_ = 0, size_ = 0;

  public:
    void push(const T &v) {
        data_[head_] = v;
        head_ = (head_ + 1) % N;
        if (size_ < N)
            ++size_;
    }
    size_t size() const {
        return size_;
    }
    const T &at(size_t i) const {
        return data_[(head_ + N - size_ + i) % N];
    }
};
uint32_t crc32(const void *data, size_t len);
bool parseHex(const char *text, uint8_t *dst, size_t bytes);
int32_t dayKey(time_t t);
time_t dayStart(time_t t);
// Inverse of dayKey: local midnight for a YYYYMMDD key (e.g. from Record::day or an /api/history
// ?date= parameter).
time_t dayStartFromKey(int32_t day);
class APSystemsDecoder {
    InverterModel configured_ = InverterModel::Auto, detected_ = InverterModel::Auto;
    bool previous_ = false;
    uint16_t seconds_ = 0;
    std::array<uint32_t, maxChannels> energy_{};
    uint64_t ms_ = 0;

  public:
    // Payload starts at the six-byte inverter serial, never at the ZNP envelope.
    bool decode(const uint8_t *p, size_t n, const uint8_t serial[6], InverterState &out);
    void setModel(InverterModel model) {
        if (configured_ != model) {
            configured_ = model;
            reset();
        }
    }
    InverterModel detectedModel() const {
        return detected_;
    }
    void reset() {
        previous_ = false;
        detected_ = InverterModel::Auto;
        energy_ = {};
        seconds_ = 0;
        ms_ = 0;
    }
};
class EnergyIntegrator {
    bool valid_ = false;
    float power_ = 0;
    uint64_t ms_ = 0;

  public:
    double add(float power, uint64_t ms, bool online, uint32_t maxGapMs = 15000);
};
enum class Resolution : uint8_t { Minute = 1, Quarter = 2, Day = 3 };
// Explicitly serialized little endian; no compiler struct layout written to flash.
struct Record {
    char serial[13] = {}; // Empty only for legacy records without an inverter identity.
    uint64_t sequence = 0, timestamp = 0;
    Resolution resolution = Resolution::Minute;
    uint8_t flags = 0; // 1=simulated
    uint32_t duration = 0, coverage = 0;
    int32_t day = 0;
    uint8_t channelCount = 0;
    std::array<float, maxChannels> channels = {missing, missing, missing, missing};
    float peak = 0;
    uint64_t peakTime = 0;
    double energyWh = 0, totalWh = 0, dayWh = 0;
};
constexpr size_t recordBytes = 96;
using EncodedRecord = std::array<uint8_t, recordBytes>;
EncodedRecord encode(const Record &r);
bool decode(const EncodedRecord &bytes, Record &r);
class BlockDevice {
  public:
    virtual ~BlockDevice() = default;
    virtual bool read(size_t off, void *data, size_t n) = 0;
    virtual bool write(size_t off, const void *data, size_t n) = 0;
    virtual bool erase(size_t off, size_t n) = 0;
};
class Journal {
    BlockDevice &dev_;
    size_t base_, sectors_, next_ = 0;
    uint64_t sequence_ = 0;
    size_t count_ = 0;
    static constexpr size_t perSector = 4096 / recordBytes;
    size_t offset(size_t slot) const {
        return base_ + (slot / perSector) * 4096 + (slot % perSector) * recordBytes;
    }

  public:
    Journal(BlockDevice &d, size_t base, size_t sectors) : dev_(d), base_(base), sectors_(sectors) {}
    bool recover();
    bool append(Record r);
    bool latest(Record &r);
    bool visit(const std::function<bool(const Record &)> &fn);
    size_t count() const {
        return count_;
    }
    size_t capacity() const {
        return sectors_ * perSector;
    }
    size_t nextSlot() const {
        return next_;
    }
    uint64_t lastSequence() const {
        return sequence_;
    }
    bool readSlot(size_t slot, Record &record, bool &valid);
};
struct Totals {
    double totalWh = 0, dayWh = 0;
    float peak = 0;
    uint64_t peakTime = 0;
    int32_t day = 0;
};
struct StatisticsSummary {
    double todayWh = missing, yesterdayWh = missing, weekWh = 0, monthWh = 0, yearWh = 0, totalWh = 0,
           bestDayWh = missing, averageWh = missing;
    float peakToday = missing;
    uint64_t peakTime = 0;
    int32_t bestDay = 0;
    unsigned completedDays = 0;
};
class StatisticsAccumulator {
    StatisticsSummary result_;
    int32_t today_ = 0, yesterday_ = 0, week_ = 0;
    double sum_ = 0;

  public:
    StatisticsAccumulator(const Totals &totals, time_t now);
    void add(const Record &record);
    StatisticsSummary result() const;
};
class Aggregator {
    struct Bucket {
        uint64_t start = 0;
        std::array<double, maxChannels> power{};
        uint8_t channelCount = 0;
        double wh = 0;
        uint32_t samples = 0, coverage = 0;
        float peak = 0;
        uint64_t peakTime = 0;
    } minute_, quarter_;
    Totals totals_{};
    uint64_t previousMs_ = 0, lastEpoch_ = 0;
    void finish(Bucket &b, Resolution res, uint64_t end, const std::function<void(const Record &)> &emit);

  public:
    void restore(const Record &r);
    void add(const InverterState &s, const std::function<void(const Record &)> &emit);
    Totals totals() const {
        return totals_;
    }
};
struct WifiPolicy {
    static bool apActive(bool enabled, bool connected) {
        return enabled || !connected;
    }
    static uint32_t retryDelay(unsigned failures) {
        return failures >= 5 ? 30000U : (1000U << failures);
    }
};
struct TransmitPolicy {
    template <typename Transmit> static bool send(bool passive, Transmit tx) {
        return passive ? false : tx();
    }
};
} // namespace sol
