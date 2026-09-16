#include "sniffer_core.hpp"
#include <algorithm>
#include <cstdio>
#include <vector>
namespace sol {
static uint64_t readLE(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
const char *classificationName(Classification c) {
    static const char *names[] = {"UNKNOWN",
                                  "IEEE802154",
                                  "ZIGBEE",
                                  "APSYSTEMS_UNKNOWN",
                                  "APSYSTEMS_ECU_TO_INVERTER",
                                  "APSYSTEMS_INVERTER_TO_ECU",
                                  "APSYSTEMS_PAIR",
                                  "APSYSTEMS_POLL",
                                  "APSYSTEMS_RESPONSE",
                                  "APSYSTEMS_FRAGMENT_PENDING",
                                  "APSYSTEMS_FRAGMENT_ORPHAN",
                                  "APSYSTEMS_FRAGMENT_DUPLICATE"};
    return names[unsigned(c)];
}
// Classifies a COMPLETE logical APS payload (pay/len point at the APS payload itself, not the
// raw 802.15.4 frame). Must never be called with a lone, unreassembled fragment: partial data
// must not be pattern-matched against known APsystems message signatures.
static void classifyPayload(ParsedFrame &f, const uint8_t *pay, size_t len) {
    f.classification = Classification::APSUnknown;
    if (f.apsType != 0)
        return;
    if (f.cluster == 0x020d || f.cluster == 0x020c || f.cluster == 0x010f ||
        (f.cluster == 0x0101 && len <= 20)) {
        f.classification = Classification::Pair;
        return;
    }
    if (len >= 10 && pay[6] == 0xfb && pay[7] == 0xfb) {
        f.classification = f.nwkSrc == 0 ? Classification::ECUToInverter : Classification::InverterToECU;
        if (f.cluster == 6 && len == 19 && pay[8] == 6 && pay[9] == 0xbb && pay[len - 2] == 0xfe &&
            pay[len - 1] == 0xfe)
            f.classification = Classification::Poll;
        if (f.cluster == 0x0106 && len >= 11 && pay[8] == 0x5c && pay[9] == 0xbb && pay[10] == 0xbb)
            f.classification = Classification::Response;
        if (f.classification == Classification::Response)
            for (int i = 0; i < 6; i++)
                snprintf(f.serial + i * 2, 3, "%02X", pay[i]);
    }
}
ParsedFrame classifyReassembled(const ParsedFrame &header, const uint8_t *payload, size_t length) {
    ParsedFrame f = header;
    f.serial[0] = 0;
    f.payloadOffset = 0;
    f.payloadLength = length;
    classifyPayload(f, payload, length);
    return f;
}
ParsedFrame parseFrame(const uint8_t *p, size_t n) {
    ParsedFrame f;
    if (n < 2)
        return f;
    f.control = readLE(p, 2);
    f.type = f.control & 7;
    f.version = (f.control >> 12) & 3;
    f.srcMode = f.control >> 14;
    f.dstMode = (f.control >> 10) & 3;
    f.encrypted = f.control & 8;
    size_t k = 2;
    f.classification = Classification::IEEE802154;
    if (f.version > 1) {
        f.unparsed = true;
        return f;
    } // 2015 PAN-presence tables/IEs are retained raw, never guessed.
    if (f.srcMode == 1 || f.dstMode == 1 || n < 3) {
        f.unparsed = true;
        return f;
    }
    f.sequence = p[k++];
    f.sequenceValid = true;
    auto take = [&](size_t count, uint64_t &value) {
        if (k + count > n)
            return false;
        value = readLE(p + k, count);
        k += count;
        return true;
    };
    uint64_t v;
    if (f.dstMode) {
        if (!take(2, v))
            return f;
        f.dstPan = v;
        f.dstPanValid = true;
        if (!take(f.dstMode == 2 ? 2 : 8, f.dst))
            return f;
    }
    if (f.srcMode) {
        if (f.control & 0x40) {
            if (!f.dstPanValid)
                return f;
            f.srcPan = f.dstPan;
            f.srcPanValid = true;
        } else {
            if (!take(2, v))
                return f;
            f.srcPan = v;
            f.srcPanValid = true;
        }
        if (!take(f.srcMode == 2 ? 2 : 8, f.src))
            return f;
    }
    f.valid = true;
    f.payloadOffset = k;
    f.payloadLength = n - k;
    if (f.encrypted || f.type != 1 || k + 8 > n)
        return f;
    uint16_t nwk = readLE(p + k, 2);
    if (((nwk >> 2) & 15) != 2 || (nwk & 3) > 1)
        return f;
    f.nwkValid = true;
    f.classification = Classification::Zigbee;
    f.nwkDst = readLE(p + k + 2, 2);
    f.nwkSrc = readLE(p + k + 4, 2);
    k += 8;
    if (nwk & 0x0800)
        k += 8;
    if (nwk & 0x1000)
        k += 8;
    if (nwk & 0x0400) {
        if (k + 2 > n) {
            f.unparsed = true;
            return f;
        }
        k += 2 + 2 * p[k];
    }
    if (k > n) {
        f.unparsed = true;
        return f;
    }
    if (nwk & 0x0200) {
        f.encrypted = true;
        return f;
    }
    if (nwk & 3)
        return f;
    if (k + 8 > n) {
        f.unparsed = true;
        return f;
    }
    uint8_t aps = p[k];
    f.apsType = aps & 3;
    if ((f.apsType != 0 && f.apsType != 2) || (aps & 0x0c) == 0x0c || (aps & 0x0c) == 4 || (aps & 0x10)) {
        f.unparsed = true;
        return f;
    }
    f.dstEp = p[k + 1];
    f.cluster = readLE(p + k + 2, 2);
    f.profile = readLE(p + k + 4, 2);
    f.srcEp = p[k + 6];
    f.apsCounter = p[k + 7];
    f.apsValid = true;
    k += 8;
    f.extendedAps = aps & 0x80;
    if (f.extendedAps) {
        if (k >= n) {
            f.unparsed = true;
            return f;
        }
        f.fragmentation = p[k++] & 3;
        if (f.fragmentation == 3) {
            f.unparsed = true;
            return f;
        }
        if (f.fragmentation) {
            if (k >= n) {
                f.unparsed = true;
                return f;
            }
            f.fragmentBlock = p[k++];
            if (f.apsType == 2) {
                if (k >= n) {
                    f.unparsed = true;
                    return f;
                }
                f.blockAck = p[k++];
            }
        }
    }
    if (aps & 0x20) {
        f.encrypted = true;
        return f;
    }
    f.payloadOffset = k;
    f.payloadLength = n - k;
    if (f.profile != 0x0f05 || f.dstEp != 0x14 || f.srcEp != 0x14)
        return f;
    if (f.fragmentation) {
        // This single 802.15.4 frame carries only one fragment of a larger APS message.
        // Pattern-matching known APsystems signatures against partial data would misclassify
        // an incomplete transfer as a resolved message type. Only classifyReassembled(), run
        // on the fully reassembled payload, may resolve a fragmented message.
        f.classification = Classification::ApsFragmentPending;
        return f;
    }
    classifyPayload(f, p + k, f.payloadLength);
    return f;
}
APSReassembler::Outcome APSReassembler::add(const CapturedFrame &frame, APSPayload &out) {
    out.length = 0;
    if (frame.length > frame.bytes.size())
        return Outcome::Conflict;
    auto h = parseFrame(frame.bytes.data(), frame.length);
    if (!h.apsValid || h.unparsed || h.encrypted || h.apsType != 0 || h.profile != 0x0f05 ||
        h.srcEp != 0x14 || h.dstEp != 0x14 || h.payloadOffset + h.payloadLength > frame.length)
        return Outcome::Conflict; // Not usable APS traffic at all: not counted in stats_.
    ++stats_.total;
    const uint8_t *data = frame.bytes.data() + h.payloadOffset;
    if (!h.fragmentation) {
        ++stats_.nonFragmented;
        out.header = h;
        out.length = h.payloadLength;
        std::copy_n(data, out.length, out.bytes.begin());
        return Outcome::Ready;
    }
    ++stats_.fragmented;
    bool isFirst = h.fragmentation == 1;
    Message *m = nullptr;
    for (auto &v : messages_) {
        if (v.used && (frame.monotonicUs < v.lastUs || frame.monotonicUs - v.lastUs > 10000000)) {
            if (!v.completed) {
                ++stats_.timeouts;
                if (onTimeout)
                    onTimeout(v.header.srcPan, v.header.nwkSrc);
            }
            v.used = false;
        }
        const auto &a = v.header;
        if (v.used && v.channel == frame.channel && a.srcPan == h.srcPan && a.nwkSrc == h.nwkSrc &&
            a.nwkDst == h.nwkDst && a.profile == h.profile && a.cluster == h.cluster && a.srcEp == h.srcEp &&
            a.dstEp == h.dstEp && a.apsCounter == h.apsCounter)
            m = &v;
    }
    // A continuation fragment that has to open a brand-new slot has no known first fragment
    // for its key: it is an orphan, not merely "pending" (which implies known context).
    bool freshSlot = !m;
    if (!m) {
        for (auto &v : messages_)
            if (!v.used) {
                m = &v;
                break;
            }
        if (!m)
            m = &*std::min_element(messages_.begin(), messages_.end(),
                                   [](const Message &a, const Message &b) { return a.lastUs < b.lastUs; });
        *m = {};
        m->used = true;
        m->header = h;
        m->channel = frame.channel;
    }
    m->lastUs = frame.monotonicUs;
    if (m->completed)
        return Outcome::Duplicate;
    if (m->conflict)
        return Outcome::Conflict;
    unsigned index = isFirst ? 0 : h.fragmentBlock;
    if (isFirst) {
        if (!h.fragmentBlock || h.fragmentBlock > 4 || (m->count && m->count != h.fragmentBlock)) {
            m->conflict = true;
            ++stats_.conflicts;
            return Outcome::Conflict;
        }
        m->count = h.fragmentBlock;
    }
    if (index >= 4 || (m->count && index >= m->count) || h.payloadLength > 125) {
        m->conflict = true;
        ++stats_.conflicts;
        return Outcome::Conflict;
    }
    if (m->mask & (1U << index)) {
        if (m->lengths[index] != h.payloadLength || memcmp(m->blocks[index].data(), data, h.payloadLength)) {
            m->conflict = true;
            ++stats_.conflicts;
            return Outcome::Conflict;
        }
        ++stats_.duplicateFragments;
        return Outcome::Duplicate;
    }
    m->lengths[index] = h.payloadLength;
    std::copy_n(data, h.payloadLength, m->blocks[index].begin());
    m->mask |= 1U << index;
    if (!m->count || m->mask != ((1U << m->count) - 1)) {
        if (freshSlot && !isFirst) {
            ++stats_.orphanFragments;
            return Outcome::Orphan;
        }
        return Outcome::Pending;
    }
    out.header = h;
    for (size_t i = 0; i < m->count; i++) {
        std::copy_n(m->blocks[i].begin(), m->lengths[i], out.bytes.begin() + out.length);
        out.length += m->lengths[i];
    }
    m->completed = true;
    ++stats_.reassembled;
    return Outcome::Ready;
}
void RadioObservability::observe(const ParsedFrame &header, uint64_t monotonicUs, uint8_t channel, int rssi,
                                 uint8_t lqi, APSReassembler::Outcome outcome, bool fragmented) {
    if (header.valid && header.srcMode == 2) {
        PhysicalTransmitter *p = nullptr;
        for (auto &v : physical_)
            if (v.used && v.mac == header.src)
                p = &v;
        if (!p)
            for (auto &v : physical_)
                if (!v.used) {
                    p = &v;
                    break;
                }
        if (!p)
            ++physicalOverflow_;
        else {
            if (!p->used) {
                p->used = true;
                p->mac = header.src;
            }
            ++p->frames;
            p->channel = channel;
            p->rssiSum += rssi;
            p->rssiMin = std::min(p->rssiMin, rssi);
            p->rssiMax = std::max(p->rssiMax, rssi);
            p->lqiSum += lqi;
            p->lastSeenUs = monotonicUs;
        }
    }
    if (!header.nwkValid)
        return;
    LogicalSource *l = nullptr;
    for (auto &v : logical_)
        if (v.used && v.pan == header.srcPan && v.address == header.nwkSrc)
            l = &v;
    if (!l)
        for (auto &v : logical_)
            if (!v.used) {
                l = &v;
                break;
            }
    if (!l)
        ++logicalOverflow_;
    else {
        if (!l->used) {
            l->used = true;
            l->pan = header.srcPan;
            l->address = header.nwkSrc;
        }
        ++l->logicalPackets;
        l->lastSeenUs = monotonicUs;
        if (fragmented) {
            switch (outcome) {
            case APSReassembler::Outcome::Ready:
                ++l->fragmentedMessages;
                ++l->completeMessages;
                l->lastCompleteMessageUs = monotonicUs;
                break;
            case APSReassembler::Outcome::Pending:
                ++l->fragmentedMessages;
                break;
            case APSReassembler::Outcome::Duplicate:
                ++l->duplicateFragments;
                break;
            case APSReassembler::Outcome::Orphan:
            case APSReassembler::Outcome::Conflict:
                ++l->orphanFragments;
                break;
            }
        } else if (outcome == APSReassembler::Outcome::Ready) {
            ++l->completeMessages;
            l->lastCompleteMessageUs = monotonicUs;
        }
    }
    if (header.srcMode == 2 && header.src != header.nwkSrc) {
        Relay *r = nullptr;
        for (auto &v : relays_)
            if (v.used && v.nwkSrc == header.nwkSrc && v.mac == header.src)
                r = &v;
        if (!r)
            for (auto &v : relays_)
                if (!v.used) {
                    r = &v;
                    break;
                }
        if (!r)
            ++relayOverflow_;
        else {
            if (!r->used) {
                r->used = true;
                r->nwkSrc = header.nwkSrc;
                r->mac = header.src;
            }
            ++r->frames;
            r->lastSeenUs = monotonicUs;
        }
    }
}
void RadioObservability::noteTimeout(uint16_t pan, uint16_t nwkSrc) {
    for (auto &v : logical_)
        if (v.used && v.pan == pan && v.address == nwkSrc) {
            ++v.reassemblyTimeouts;
            return;
        }
}
static void put(std::vector<uint8_t> &b, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++)
        b.push_back(v >> (8 * i));
}
static bool block(uint32_t type, std::vector<uint8_t> body, const EmitBytes &emit) {
    while (body.size() % 4)
        body.push_back(0);
    std::vector<uint8_t> b;
    put(b, type, 4);
    put(b, body.size() + 12, 4);
    b.insert(b.end(), body.begin(), body.end());
    put(b, b.size() + 4, 4);
    return emit(b.data(), b.size());
}
static void option(std::vector<uint8_t> &b, uint16_t key, const char *text) {
    size_t n = strlen(text);
    put(b, key, 2);
    put(b, n, 2);
    b.insert(b.end(), text, text + n);
    while (b.size() % 4)
        b.push_back(0);
}
bool pcapngHeader(const EmitBytes &emit) {
    std::vector<uint8_t> b;
    put(b, 0x1a2b3c4d, 4);
    put(b, 1, 2);
    put(b, 0, 2);
    put(b, UINT64_MAX, 8);
    if (!block(0x0a0d0d0a, b, emit))
        return false;
    for (int ch = 11; ch <= 26; ch++) {
        b.clear();
        put(b, 230, 2);
        put(b, 0, 2);
        put(b, 125, 4);
        char name[40];
        snprintf(name, sizeof(name), "OpenSolECU 802.15.4 channel %d", ch);
        option(b, 2, name);
        put(b, 0, 4);
        if (!block(1, b, emit))
            return false;
    }
    return true;
}
bool pcapngFrame(const CapturedFrame &f, const EmitBytes &emit) {
    if (f.channel < 11 || f.channel > 26 || f.length > 125)
        return false;
    std::vector<uint8_t> b;
    put(b, f.channel - 11, 4);
    uint64_t t = f.epochUs ? f.epochUs : f.monotonicUs;
    put(b, t >> 32, 4);
    put(b, t & 0xffffffff, 4);
    put(b, f.length, 4);
    put(b, f.length, 4);
    b.insert(b.end(), f.bytes.begin(), f.bytes.begin() + f.length);
    while (b.size() % 4)
        b.push_back(0);
    char comment[110];
    snprintf(comment, sizeof(comment), "channel=%u RSSI=%d dBm LQI=%u timestamp=%s FCS omitted by driver",
             f.channel, f.rssi, f.lqi, f.epochUs ? "UTC" : "monotonic (unsynchronized)");
    option(b, 1, comment);
    put(b, 0, 4);
    return block(6, b, emit);
}
} // namespace sol
