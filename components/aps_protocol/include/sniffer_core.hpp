#pragma once
#include "protocol.hpp"
namespace sol {
enum class Classification : uint8_t {
    Unknown,
    IEEE802154,
    Zigbee,
    APSUnknown,
    ECUToInverter,
    InverterToECU,
    Pair,
    Poll,
    Response,
    // A complete classification (above) requires a complete APS payload. A fragment alone
    // must never be reverse-engineered as one of the types above.
    ApsFragmentPending,
    ApsFragmentOrphan,
    ApsFragmentDuplicate
};
const char *classificationName(Classification c);
struct ParsedFrame {
    bool valid = false, encrypted = false, unparsed = false, nwkValid = false, apsValid = false;
    uint16_t control = 0, srcPan = 0, dstPan = 0, nwkSrc = 0, nwkDst = 0, profile = 0, cluster = 0;
    bool srcPanValid = false, dstPanValid = false, sequenceValid = false;
    uint64_t src = 0, dst = 0;
    uint8_t srcMode = 0, dstMode = 0, type = 0, version = 0, sequence = 0, srcEp = 0, dstEp = 0,
            apsCounter = 0;
    size_t payloadOffset = 0, payloadLength = 0;
    uint8_t apsType = 0, fragmentation = 0, fragmentBlock = 0, blockAck = 0;
    bool extendedAps = false;
    Classification classification = Classification::Unknown;
    char serial[13] = {};
};
ParsedFrame parseFrame(const uint8_t *data, size_t n);
// Classifies an already-complete logical APS payload (non-fragmented single frame, or the
// output of a finished APSReassembler::add). Never call this on a lone fragment.
ParsedFrame classifyReassembled(const ParsedFrame &header, const uint8_t *payload, size_t length);
struct CapturedFrame {
    uint64_t id = 0, epochUs = 0, monotonicUs = 0;
    uint8_t channel = 0, length = 0, lqi = 0;
    int8_t rssi = 0;
    std::array<uint8_t, 125> bytes{};
    // Set once at ingest time by the reassembly-aware classifier (sniffer.cpp); reassembly
    // is sequential/stateful and cannot be recomputed later from this frame's bytes alone.
    Classification resolvedClassification = Classification::Unknown;
    char resolvedSerial[13] = {};
};
struct APSPayload {
    ParsedFrame header;
    size_t length = 0;
    std::array<uint8_t, 500> bytes{};
};
class APSReassembler {
  public:
    enum class Outcome : uint8_t {
        Ready,     // out is populated: a non-fragmented frame, or reassembly just completed.
        Pending,   // fragment accepted, transaction still incomplete.
        Duplicate, // exact repeat of an already-received block.
        Orphan,    // a continuation fragment with no known first fragment for its key.
        Conflict   // same block index received with different bytes; transaction abandoned.
    };
    struct Stats {
        uint32_t total = 0, nonFragmented = 0, fragmented = 0, reassembled = 0, duplicateFragments = 0,
                 orphanFragments = 0, conflicts = 0, timeouts = 0;
    };
    // Invoked (if set) when a pending transaction is evicted for being stale, before its slot
    // is reused. Lets a caller attribute a timeout to the logical source that owned it.
    std::function<void(uint16_t pan, uint16_t nwkSrc)> onTimeout;
    Outcome add(const CapturedFrame &frame, APSPayload &payload);
    Stats stats() const {
        return stats_;
    }

  private:
    struct Message {
        bool used = false, completed = false, conflict = false;
        uint64_t lastUs = 0;
        uint8_t channel = 0, count = 0, mask = 0;
        ParsedFrame header;
        std::array<uint8_t, 4> lengths{};
        std::array<std::array<uint8_t, 125>, 4> blocks{};
    };
    std::array<Message, 4> messages_{};
    Stats stats_{};
};
// Physical (macSrc), logical (srcPan/nwkSrc) and mesh-relay observability, kept strictly
// separate: RSSI/LQI belong to the physical transmitter, never to a logical/NWK identity.
class RadioObservability {
  public:
    struct PhysicalTransmitter {
        bool used = false;
        uint64_t mac = 0;
        uint8_t channel = 0;
        uint32_t frames = 0;
        int64_t rssiSum = 0;
        int rssiMin = 127, rssiMax = -128;
        int64_t lqiSum = 0;
        uint64_t lastSeenUs = 0;
    };
    struct LogicalSource {
        bool used = false;
        uint16_t pan = 0, address = 0;
        uint32_t logicalPackets = 0, completeMessages = 0, fragmentedMessages = 0, duplicateFragments = 0,
                 orphanFragments = 0, reassemblyTimeouts = 0;
        uint64_t lastSeenUs = 0, lastCompleteMessageUs = 0;
    };
    struct Relay {
        bool used = false;
        uint16_t nwkSrc = 0;
        uint64_t mac = 0;
        uint32_t frames = 0;
        uint64_t lastSeenUs = 0;
    };
    // header must be nwkValid. outcome reflects the same frame's APSReassembler::add result
    // when it was fragmented APS traffic (any value is fine for non-fragmented/non-APS frames).
    void observe(const ParsedFrame &header, uint64_t monotonicUs, uint8_t channel, int rssi, uint8_t lqi,
                 APSReassembler::Outcome outcome, bool fragmented);
    void noteTimeout(uint16_t pan, uint16_t nwkSrc);
    const std::array<PhysicalTransmitter, 24> &physical() const {
        return physical_;
    }
    const std::array<LogicalSource, 24> &logical() const {
        return logical_;
    }
    const std::array<Relay, 48> &relays() const {
        return relays_;
    }
    uint32_t physicalOverflow() const {
        return physicalOverflow_;
    }
    uint32_t logicalOverflow() const {
        return logicalOverflow_;
    }
    uint32_t relayOverflow() const {
        return relayOverflow_;
    }

  private:
    std::array<PhysicalTransmitter, 24> physical_{};
    std::array<LogicalSource, 24> logical_{};
    std::array<Relay, 48> relays_{};
    uint32_t physicalOverflow_ = 0, logicalOverflow_ = 0, relayOverflow_ = 0;
};
// Wireshark LINKTYPE_IEEE802_15_4_NOFCS (230). Per-channel interfaces and RSSI/LQI comments.
using EmitBytes = std::function<bool(const uint8_t *, size_t)>;
bool pcapngHeader(const EmitBytes &emit);
bool pcapngFrame(const CapturedFrame &f, const EmitBytes &emit);
} // namespace sol
