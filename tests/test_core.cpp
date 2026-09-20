#include "protocol.hpp"
#include "sniffer_core.hpp"
#include "sol.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
using namespace sol;
using Outcome = APSReassembler::Outcome;
static unsigned checks = 0;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x))                                                                                            \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x);       \
    } while (false)
static bool near(double a, double b, double tolerance = 1e-5) {
    return std::abs(a - b) < tolerance;
}
static std::vector<uint8_t> hex(const std::string &s) {
    std::vector<uint8_t> b(s.size() / 2);
    CHECK(parseHex(s.c_str(), b.data(), b.size()));
    return b;
}
static std::vector<uint8_t> fixture(const char *path) {
    std::ifstream f(path);
    std::string s;
    f >> s;
    CHECK(!s.empty());
    return hex(s);
}
static void checksum(std::vector<uint8_t> &b) {
    unsigned sum = 0;
    for (size_t i = 8; i < b.size() - 4; i++)
        sum += b[i];
    b[b.size() - 4] = sum >> 8;
    b[b.size() - 3] = sum;
}
static void be32(std::vector<uint8_t> &b, size_t pos, uint32_t v) {
    for (int i = 3; i >= 0; i--) {
        b[pos + i] = v;
        v >>= 8;
    }
}
static void be24(std::vector<uint8_t> &b, size_t pos, uint32_t v) {
    b[pos] = v >> 16;
    b[pos + 1] = v >> 8;
    b[pos + 2] = v;
}
static void fragments() {
    auto payload = fixture("tests/fixtures/ds3-0.hex");
    auto prefix = hex("618801d8a3000071540800000071540f01c0140601050f140d");
    auto frame = [&](bool first) {
        CapturedFrame f;
        f.channel = 16;
        f.monotonicUs = 1000000;
        std::copy(prefix.begin(), prefix.end(), f.bytes.begin());
        f.bytes[25] = first ? 1 : 2;
        f.bytes[26] = first ? 2 : 1;
        size_t start = first ? 0 : 97, n = first ? 97 : payload.size() - 97;
        std::copy_n(payload.begin() + start, n, f.bytes.begin() + 27);
        f.length = uint8_t(27 + n);
        return f;
    };
    auto first = frame(true), last = frame(false);
    auto header = parseFrame(first.bytes.data(), first.length);
    CHECK(header.apsValid && header.payloadOffset == 27 && header.payloadLength == 97);
    // A lone fragment -- even the first one, even though its bytes happen to contain the
    // signature that a complete Response payload would show at the same offset -- must never
    // resolve to a known APsystems message type. Only a fully reassembled payload may.
    CHECK(header.fragmentation == 1 && header.fragmentBlock == 2 &&
          header.classification == Classification::ApsFragmentPending);
    APSReassembler r;
    APSPayload out;
    CHECK(r.add(first, out) == Outcome::Pending);
    CHECK(r.add(first, out) == Outcome::Duplicate); // exact repeat of the same block.
    CHECK(r.add(last, out) == Outcome::Ready);
    CHECK(out.length == 105 && !memcmp(out.bytes.data(), payload.data(), 105));
    // Only now, on the complete reassembled payload, does the message resolve.
    auto resolved = classifyReassembled(out.header, out.bytes.data(), out.length);
    CHECK(resolved.classification == Classification::Response);
    char serial[13];
    for (int i = 0; i < 6; i++)
        snprintf(serial + 2 * i, 3, "%02X", payload[i]);
    CHECK(!strcmp(resolved.serial, serial));
    CHECK(r.add(last, out) == Outcome::Duplicate); // resend after completion.
    CHECK(r.add(first, out) == Outcome::Duplicate);
    r = {};
    CHECK(r.add(last, out) == Outcome::Orphan); // continuation with no known first fragment.
    CHECK(r.stats().orphanFragments == 1);
    auto relay = first;
    relay.bytes[7] ^= 1;
    CHECK(r.add(relay, out) == Outcome::Ready); // same logical key, different physical relay hop.
    CHECK(out.header.nwkSrc == 0x5471);
    r = {};
    CHECK(r.add(first, out) == Outcome::Pending);
    auto bad = first;
    bad.bytes[40] ^= 1;
    CHECK(r.add(bad, out) == Outcome::Conflict); // same block index, different bytes.
    CHECK(r.stats().conflicts == 1);
    CHECK(r.add(last, out) == Outcome::Conflict); // transaction stays poisoned.
    for (int offset : {3, 13, 24}) {              // PAN, originating NWK source, APS counter isolate groups.
        r = {};
        CHECK(r.add(first, out) == Outcome::Pending);
        auto other = last;
        other.bytes[offset] ^= 1;
        CHECK(r.add(other, out) == Outcome::Orphan); // different key: a brand-new, orphan slot.
        CHECK(r.add(last, out) == Outcome::Ready);   // original key unaffected: no collision.
    }
    // Capacity: maxTransactions (8) distinct pending transactions coexist without evicting one
    // another; only a 9th distinct one forces an eviction, and only the least-recently-touched
    // slot (lowest monotonicUs) is the one that goes.
    r = {};
    std::array<CapturedFrame, APSReassembler::maxTransactions> firsts;
    for (size_t i = 0; i < firsts.size(); i++) {
        firsts[i] = first;
        firsts[i].bytes[24] ^= uint8_t(i + 1); // distinct apsCounter per slot.
        firsts[i].monotonicUs = 1000000 + uint64_t(i) * 1000;
        CHECK(r.add(firsts[i], out) == Outcome::Pending);
    }
    auto ninth = first;
    ninth.bytes[24] ^= 99;
    ninth.monotonicUs = firsts.back().monotonicUs + 1000;
    CHECK(r.add(ninth, out) == Outcome::Pending); // 9th key, all 8 slots full: evicts slot 0.
    // Slots 1..7 were untouched by that eviction: each still completes normally. Check this
    // before touching slot 0 again below -- completing a matching slot never evicts anything
    // (only a *fresh*, non-matching key can), so this loop alone can't perturb slots 1..7.
    for (size_t i = 1; i < firsts.size(); i++) {
        auto lastVariant = last;
        lastVariant.bytes[24] ^= uint8_t(i + 1);
        lastVariant.monotonicUs = ninth.monotonicUs + uint64_t(i) * 100 + 100;
        CHECK(r.add(lastVariant, out) == Outcome::Ready);
    }
    // Slot 0's original context is gone: completing it now looks like a fresh continuation
    // with no known first fragment, never a silent success.
    auto lastForFirst0 = last;
    lastForFirst0.bytes[24] ^= uint8_t(1);
    lastForFirst0.monotonicUs = ninth.monotonicUs + 8 * 100 + 200;
    CHECK(r.add(lastForFirst0, out) != Outcome::Ready);
    r = {};
    CHECK(r.add(first, out) == Outcome::Pending);
    auto late = last;
    late.monotonicUs += APSReassembler::timeoutUs + 1000000; // 1s past the timeout, not 10s flat.
    CHECK(r.add(late, out) == Outcome::Orphan); // original slot timed out; late opens a new one.
    CHECK(r.stats().timeouts == 1);
    r = {};
    auto ack = first;
    ack.bytes[17] = 0x82;
    ack.bytes[26] = 0;
    ack.bytes[27] = 255;
    ack.length = 28;
    CHECK(parseFrame(ack.bytes.data(), ack.length).apsType == 2);
    CHECK(r.add(ack, out) == Outcome::Conflict); // rejected before touching any transaction.
    CHECK(r.add(last, out) == Outcome::Orphan);
    CHECK(r.add(first, out) == Outcome::Ready);
    for (size_t n = 0; n < first.length; n++) {
        r = {};
        auto shortFrame = first;
        shortFrame.length = uint8_t(n);
        r.add(shortFrame, out);
        if (r.add(last, out) == Outcome::Ready) {
            APSystemsDecoder decoder;
            InverterState s;
            CHECK(!decoder.decode(out.bytes.data(), out.length, payload.data(), s));
        }
    }
    r = {};
    bad = first;
    bad.bytes[26] = 255;
    CHECK(r.add(bad, out) == Outcome::Conflict);
    CHECK(r.add(last, out) == Outcome::Conflict);
    // Separate decoder baselines: a second DS3's counters cannot affect the first.
    std::array<APSystemsDecoder, 3> decoders;
    std::array<InverterState, 3> states;
    for (int i = 0; i < 3; i++) {
        auto p = payload;
        p[5] += i;
        states[i].maximumGapMs = 900000;
        states[i].monotonicMs = 1000;
        CHECK(decoders[i].decode(p.data(), p.size(), p.data(), states[i]));
        CHECK(std::isnan(states[i].totalPower));
    }
    auto next = payload;
    be32(next, 50, states[0].rawEnergy[0] + 100000);
    be32(next, 54, states[0].rawEnergy[1] + 120000);
    uint16_t seconds = states[0].inverterSeconds + 300;
    next[38] = seconds >> 8;
    next[39] = seconds;
    checksum(next);
    states[0].monotonicMs = 301000;
    CHECK(decoders[0].decode(next.data(), next.size(), next.data(), states[0]));
    CHECK(near(states[0].totalPower, 220000 * .0000166 * 3600 / 300, 1e-4));
    CHECK(std::isnan(states[1].totalPower));
}
static void legacyModels() {
    struct Sample {
        const char *path;
        InverterModel model;
        uint8_t channels;
        size_t secondsOffset;
    };
    for (const auto &sample : {Sample{"tests/fixtures/yc600.hex", InverterModel::YC600, 2, 17},
                               Sample{"tests/fixtures/qs1.hex", InverterModel::QS1, 4, 30}}) {
        auto payload = fixture(sample.path);
        CHECK(payload.size() == 94);
        APSystemsDecoder decoder;
        InverterState state;
        state.maximumGapMs = 15000;
        state.monotonicMs = 1000;
        CHECK(decoder.decode(payload.data(), payload.size(), payload.data(), state));
        CHECK(state.model == sample.model && decoder.detectedModel() == sample.model);
        CHECK(state.channelCount == sample.channels && std::isnan(state.totalPower));
        CHECK(state.acVoltage > 200 && state.acVoltage < 250);
        CHECK(state.acFrequency > 49 && state.acFrequency < 51);
        for (size_t channel = 0; channel < sample.channels; channel++) {
            CHECK(state.channels[channel].voltage > 0 && state.channels[channel].voltage < 80);
            CHECK(state.channels[channel].current >= 0 && state.channels[channel].current < 30);
        }
        auto next = payload;
        for (size_t channel = 0; channel < sample.channels; channel++)
            be24(next, 37 + channel * 5, state.rawEnergy[channel] + 10);
        uint16_t seconds = state.inverterSeconds + 10;
        next[sample.secondsOffset] = seconds >> 8;
        next[sample.secondsOffset + 1] = seconds;
        state.monotonicMs = 11000;
        CHECK(decoder.decode(next.data(), next.size(), next.data(), state));
        CHECK(near(state.totalPower, sample.channels * 8.311, 1e-3));
        APSystemsDecoder forced;
        forced.setModel(sample.model);
        InverterState forcedState;
        CHECK(forced.decode(payload.data(), payload.size(), payload.data(), forcedState));
        APSystemsDecoder wrong;
        wrong.setModel(InverterModel::DS3);
        CHECK(!wrong.decode(payload.data(), payload.size(), payload.data(), forcedState));
    }
    CHECK(inferInverterModel("703000021300") == InverterModel::DS3);
    CHECK(inferInverterModel("408000158215") == InverterModel::YC600);
    CHECK(inferInverterModel("801000085070") == InverterModel::QS1);
}
static void radioObservability() {
    RadioObservability r;
    ParsedFrame direct; // macSrc == nwkSrc: a genuine direct reception, no relay involved.
    direct.valid = true;
    direct.srcMode = 2;
    direct.src = 0x1234;
    direct.nwkValid = true;
    direct.srcPan = 0xabcd;
    direct.nwkSrc = 0x1234;
    r.observe(direct, 1000, 16, -50, 200, Outcome::Ready, false);
    r.observe(direct, 2000, 16, -60, 180, Outcome::Ready, false);
    CHECK(r.physical()[0].used && r.physical()[0].mac == 0x1234 && r.physical()[0].frames == 2);
    CHECK(near(double(r.physical()[0].rssiSum) / 2, -55));
    CHECK(r.physical()[0].rssiMin == -60 && r.physical()[0].rssiMax == -50);
    CHECK(r.logical()[0].used && r.logical()[0].pan == 0xabcd && r.logical()[0].address == 0x1234);
    CHECK(r.logical()[0].logicalPackets == 2 && r.logical()[0].completeMessages == 2);
    unsigned relayCount = 0;
    for (auto &rel : r.relays())
        if (rel.used)
            ++relayCount;
    CHECK(relayCount == 0); // mac == nwkSrc: no relay to report.
    ParsedFrame relayed = direct;
    relayed.src = 0x5678; // same logical source, heard this time via a different physical hop.
    r.observe(relayed, 3000, 16, -90, 50, Outcome::Ready, false);
    relayCount = 0;
    bool foundRelay = false;
    for (auto &rel : r.relays())
        if (rel.used) {
            ++relayCount;
            foundRelay |= rel.nwkSrc == 0x1234 && rel.mac == 0x5678;
        }
    CHECK(relayCount == 1 && foundRelay);
    const RadioObservability::PhysicalTransmitter *direct1234 = nullptr, *relay5678 = nullptr;
    for (auto &p : r.physical()) {
        if (p.used && p.mac == 0x1234)
            direct1234 = &p;
        if (p.used && p.mac == 0x5678)
            relay5678 = &p;
    }
    // The relay's RSSI lands on its own physical entry, never blended into the direct one's.
    CHECK(direct1234 && direct1234->frames == 2);
    CHECK(relay5678 && relay5678->frames == 1 && relay5678->rssiMin == -90 && relay5678->rssiMax == -90);
    CHECK(r.logical()[0].logicalPackets == 3 && r.logical()[0].completeMessages == 3);
    RadioObservability r2;
    ParsedFrame frag = direct;
    r2.observe(frag, 1000, 16, -70, 100, Outcome::Pending, true);
    CHECK(r2.logical()[0].fragmentedMessages == 1 && r2.logical()[0].completeMessages == 0);
    r2.observe(frag, 2000, 16, -70, 100, Outcome::Duplicate, true);
    CHECK(r2.logical()[0].duplicateFragments == 1);
    r2.observe(frag, 3000, 16, -70, 100, Outcome::Orphan, true);
    CHECK(r2.logical()[0].orphanFragments == 1);
    r2.observe(frag, 4000, 16, -70, 100, Outcome::Ready, true);
    CHECK(r2.logical()[0].completeMessages == 1 && r2.logical()[0].fragmentedMessages == 2);
    r2.noteTimeout(0xabcd, 0x1234);
    CHECK(r2.logical()[0].reassemblyTimeouts == 1);
    // A frame with no NWK layer never creates a logical-source entry (no RSSI field exists
    // there to blend anyway), but still populates the physical table from the raw reception.
    RadioObservability r3;
    ParsedFrame noNwk;
    noNwk.valid = true;
    noNwk.srcMode = 2;
    noNwk.src = 0x9999;
    r3.observe(noNwk, 1000, 16, -40, 255, Outcome::Conflict, false);
    CHECK(r3.physical()[0].used && r3.physical()[0].mac == 0x9999);
    unsigned logicalUsed = 0;
    for (auto &l : r3.logical())
        if (l.used)
            ++logicalUsed;
    CHECK(logicalUsed == 0);
}
class Nor : public BlockDevice {
  public:
    std::vector<uint8_t> memory;
    int tear = -1;
    explicit Nor(size_t size) : memory(size, 255) {}
    bool read(size_t o, void *p, size_t n) override {
        CHECK(o + n <= memory.size());
        memcpy(p, memory.data() + o, n);
        return true;
    }
    bool write(size_t o, const void *p, size_t n) override {
        auto b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; i++) {
            if (tear == 0) {
                tear = -1;
                return false;
            }
            if (tear > 0)
                --tear;
            CHECK((memory[o + i] & b[i]) == b[i]);
            memory[o + i] &= b[i];
        }
        return true;
    }
    bool erase(size_t o, size_t n) override {
        CHECK(o % 4096 == 0 && n % 4096 == 0 && o + n <= memory.size());
        std::fill(memory.begin() + o, memory.begin() + o + n, 255);
        return true;
    }
};
static int replay(const char *path) {
    std::ifstream input(path);
    if (!input)
        return 2;
    APSReassembler assembler;
    std::map<std::string, APSystemsDecoder> decoders;
    uint64_t us;
    unsigned channel;
    std::string raw;
    unsigned complete = 0, valid = 0;
    while (input >> us >> channel >> raw) {
        CapturedFrame f;
        f.monotonicUs = us;
        f.channel = uint8_t(channel);
        if (raw.size() > 250 || raw.size() % 2 || !parseHex(raw.c_str(), f.bytes.data(), raw.size() / 2))
            return 3;
        f.length = uint8_t(raw.size() / 2);
        APSPayload p;
        if (assembler.add(f, p) != Outcome::Ready || p.header.cluster != 0x0106)
            continue;
        ++complete;
        char serial[13];
        for (int i = 0; i < 6; i++)
            snprintf(serial + 2 * i, 3, "%02X", p.bytes[i]);
        InverterState state;
        state.monotonicMs = us / 1000;
        state.maximumGapMs = 900000;
        if (!decoders[serial].decode(p.bytes.data(), p.length, p.bytes.data(), state))
            continue;
        ++valid;
        std::cout << serial << " V1=" << state.channels[0].voltage << " V2=" << state.channels[1].voltage
                  << " W=" << state.totalPower << "\n";
    }
    std::cout << "REPLAY complete=" << complete << " valid=" << valid << " inverters=" << decoders.size()
              << "\n";
    return complete == valid && valid > 0 ? 0 : 1;
}
int main(int argc, char **argv) {
    if (argc == 2)
        return replay(argv[1]);
    try {
#ifdef _WIN32
        _putenv_s("TZ", "UTC0");
        _tzset();
#else
        setenv("TZ", "UTC0", 1);
        tzset();
#endif
        CHECK(crc32("123456789", 9) == 0xcbf43926);
        time_t dayKeyProbe = 1704067200 + 12345; // 2024-01-01 03:25:45 UTC
        CHECK(dayStartFromKey(dayKey(dayKeyProbe)) == dayStart(dayKeyProbe));
        CHECK(dayKey(dayStartFromKey(20240101)) == 20240101);
        CHECK(dayKey(dayStartFromKey(20241231)) == 20241231);
        fragments();
        legacyModels();
        radioObservability();
        uint8_t id[6];
        CHECK(parseHex("703000021300", id, 6));
        CHECK(!parseHex("123", id, 6));
        CHECK(!parseHex("GG3000021300", id, 6));
        Ring<int, 3> ring;
        for (int i = 0; i < 8; i++)
            ring.push(i);
        CHECK(ring.size() == 3 && ring.at(0) == 5 && ring.at(2) == 7);
        EnergyIntegrator energy;
        CHECK(energy.add(100, 1000, true) == 0);
        CHECK(near(energy.add(300, 6000, true), 200 * 5 / 3600.0));
        CHECK(energy.add(300, 26000, true) == 0);
        CHECK(energy.add(0, 27000, false) == 0);
        CHECK(energy.add(300, 28000, true) == 0);
        CHECK(energy.add(missing, 29000, true) == 0);
        CHECK(energy.add(300, 30000, true) == 0);
        CHECK(energy.add(300, 29000, true) == 0);
        auto b = fixture("tests/fixtures/ds3-0.hex");
        APSystemsDecoder decoder;
        InverterState s;
        s.timestamp = 1704067200;
        s.monotonicMs = 1000;
        CHECK(decoder.decode(b.data(), b.size(), id, s));
        CHECK(s.online && std::isnan(s.totalPower));
        CHECK(s.model == InverterModel::DS3 && s.channelCount == 2);
        CHECK(near(s.channels[0].voltage, 1781 / 48.0, 1e-4));
        CHECK(near(s.channels[1].voltage, 1785 / 48.0, 1e-4));
        CHECK(near(s.acVoltage, 864 / 3.8, 1e-4));
        CHECK(near(s.acFrequency, 50.02, 1e-4));
        CHECK(near(s.temperature, 1680.0 / 40.0 - 26.5, 1e-4));
        auto wrong = b;
        wrong[30] ^= 1;
        CHECK(!decoder.decode(wrong.data(), wrong.size(), id, s));
        CHECK(!decoder.decode(b.data(), 60, id, s));
        wrong = b;
        wrong[0] ^= 1;
        checksum(wrong);
        CHECK(!decoder.decode(wrong.data(), wrong.size(), id, s));
        auto next = b;
        be32(next, 50, s.rawEnergy[0] + 10000);
        be32(next, 54, s.rawEnergy[1] + 12000);
        uint16_t time = s.inverterSeconds + 5;
        next[38] = time >> 8;
        next[39] = time;
        checksum(next);
        s.monotonicMs = 6000;
        CHECK(decoder.decode(next.data(), next.size(), id, s));
        CHECK(near(s.totalPower, 22000 * .0000166 * 3600 / 5, 1e-4));
        CHECK(s.powerIntervalSeconds == 5);
        CHECK(near(s.energyDeltaWh, 22000 * .0000166));
        s.monotonicMs = 11000;
        CHECK(decoder.decode(next.data(), next.size(), id, s));
        CHECK(std::isnan(s.totalPower) && s.energyDeltaWh == 0);
        APSystemsDecoder sparseDecoder;
        InverterState sparse;
        sparse.maximumGapMs = 2U * 60U * 60U * 1000U;
        sparse.monotonicMs = 1;
        CHECK(sparseDecoder.decode(b.data(), b.size(), id, sparse));
        auto sparseNext = b;
        be32(sparseNext, 50, sparse.rawEnergy[0] + 1000000);
        be32(sparseNext, 54, sparse.rawEnergy[1] + 1200000);
        time = sparse.inverterSeconds + 1800;
        sparseNext[38] = time >> 8;
        sparseNext[39] = time;
        checksum(sparseNext);
        sparse.monotonicMs += 1800U * 1000U;
        CHECK(sparseDecoder.decode(sparseNext.data(), sparseNext.size(), id, sparse));
        CHECK(sparse.powerIntervalSeconds == 1800);
        CHECK(near(sparse.totalPower, 2200000 * .0000166 * 2, 1e-4));
        // Every truncation of the published DS3 payload is rejected.
        for (size_t n = 0; n < b.size(); n++)
            CHECK(!decoder.decode(b.data(), n, id, s));
        Record r;
        r.timestamp = 1704067200;
        r.day = 20240101;
        r.energyWh = 1.25;
        r.totalWh = 9876.5;
        r.dayWh = 12;
        strcpy(r.serial, "703000021300");
        r.channelCount = 4;
        r.channels = {123.4f, 234.5f, 345.6f, missing};
        r.sequence = 42;
        r.peak = 500;
        r.peakTime = r.timestamp;
        auto encoded = encode(r);
        Record restored;
        CHECK(decode(encoded, restored));
        CHECK(restored.sequence == 42 && near(restored.totalWh, 9876.5));
        CHECK(encoded.size() == recordBytes && encoded[0] == 4); // format version 4
        CHECK(!strcmp(restored.serial, r.serial) && restored.channelCount == 4);
        CHECK(near(restored.channels[0], 123.4) && near(restored.channels[2], 345.6) &&
              std::isnan(restored.channels[3]));
        for (size_t i = 0; i < encoded.size(); i++) {
            auto bad = encoded;
            bad[i] ^= 1;
            CHECK(!decode(bad, restored));
        }
        Nor nor(8192);
        Journal journal(nor, 0, 2);
        CHECK(journal.recover());
        for (int i = 0; i < 150; i++) {
            r.totalWh = i;
            CHECK(journal.append(r));
        }
        Record last;
        CHECK(journal.latest(last) && last.sequence == 150 && last.totalWh == 149);
        Journal recovered(nor, 0, 2);
        CHECK(recovered.recover());
        CHECK(recovered.count() >= 64 && recovered.count() <= 128);
        CHECK(recovered.latest(last) && last.sequence == 150);
        nor.tear = 45;
        r.totalWh = 999;
        CHECK(!recovered.append(r));
        Journal torn(nor, 0, 2);
        CHECK(torn.recover());
        CHECK(torn.latest(last) && last.totalWh == 149);
        CHECK(torn.append(r));
        CHECK(torn.latest(last) && last.totalWh == 999);
        // Power-loss at every byte in a record must preserve the prior committed record.
        for (size_t cut = 0; cut < recordBytes; cut++) {
            Nor flash(8192);
            Journal j(flash, 0, 2);
            CHECK(j.recover());
            CHECK(j.append(r));
            flash.tear = cut;
            CHECK(!j.append(r));
            Journal rebooted(flash, 0, 2);
            CHECK(rebooted.recover());
            CHECK(rebooted.latest(last) && last.sequence == 1);
            CHECK(rebooted.append(r));
            CHECK(rebooted.latest(last) && last.sequence == 2);
        }
        APSProtocol aps;
        auto poll = aps.poll(0x5471);
        auto captured =
            hex("6188b9d8a3715400000800715400000f4800140600050f140180971b01a3d8fbfb06bb000000000000c1fefe");
        captured[2] = 0;
        captured[16] = 0;
        captured[24] = 0;
        CHECK(poll.length == captured.size());
        CHECK(std::equal(captured.begin(), captured.end(), poll.bytes.begin()));
        APSMessage msg;
        CHECK(APSProtocol::parse(poll.bytes.data(), poll.length, msg));
        CHECK(msg.cluster == 6 && msg.profile == 0x0f05 && msg.destination == 0x5471 && msg.length == 19);
        APSProtocol pairProtocol;
        auto pair = pairProtocol.pair(0, id);
        auto pairing =
            hex("41882cffffffff00000800ffff00000f0008140d02050f1400703000021300ffff10ffff80971b01a3d8");
        pairing[2] = 0;
        CHECK(pair.length == pairing.size());
        CHECK(std::equal(pairing.begin(), pairing.end(), pair.bytes.begin()));
        auto parsed = parseFrame(poll.bytes.data(), poll.length);
        CHECK(parsed.valid && parsed.classification == Classification::Poll);
        CHECK(parsed.srcPan == 0xa3d8 && parsed.dstPan == 0xa3d8 && parsed.src == 0 && parsed.dst == 0x5471);
        CHECK(parsed.payloadOffset == 25 && parsed.payloadLength == 19);
        CHECK(parsed.control & 0x20);
        CHECK(parseFrame(pair.bytes.data(), pair.length).classification == Classification::Pair);
        auto shortFrame = hex("4188013412cdab7856010203");
        parsed = parseFrame(shortFrame.data(), shortFrame.size());
        CHECK(parsed.valid && parsed.src == 0x5678 && parsed.dst == 0xabcd && parsed.srcPan == 0x1234 &&
              parsed.payloadLength == 3);
        auto extended = hex("01cc02cdab080706050403020134120102030405060708aabb");
        parsed = parseFrame(extended.data(), extended.size());
        CHECK(parsed.valid && parsed.src == 0x0807060504030201ULL && parsed.dst == 0x0102030405060708ULL &&
              parsed.srcPan == 0x1234 && parsed.dstPan == 0xabcd && parsed.payloadLength == 2);
        auto ack = hex("020034");
        parsed = parseFrame(ack.data(), ack.size());
        CHECK(parsed.valid && parsed.type == 2 && !parsed.srcMode && parsed.payloadLength == 0);
        auto secured = poll;
        secured.bytes[0] |= 8;
        parsed = parseFrame(secured.bytes.data(), secured.length);
        CHECK(parsed.encrypted && !parsed.apsValid);
        auto unknown = poll;
        unknown.bytes[21] = 0;
        unknown.bytes[22] = 0;
        CHECK(parseFrame(unknown.bytes.data(), unknown.length).classification == Classification::Zigbee);
        // Fuzz bounded lengths/control combinations: parsers must never read past the input.
        for (unsigned fc = 0; fc < 65536; fc += 17) {
            auto f = poll;
            f.bytes[0] = fc;
            f.bytes[1] = fc >> 8;
            for (size_t n = 0; n < f.length; n++)
                parseFrame(f.bytes.data(), n);
        }
        unsigned transmissions = 0;
        CHECK(!TransmitPolicy::send(true, [&]() {
            ++transmissions;
            return true;
        }));
        CHECK(transmissions == 0);
        CHECK(TransmitPolicy::send(false, [&]() {
            ++transmissions;
            return true;
        }));
        CHECK(transmissions == 1);
        CHECK(WifiPolicy::apActive(true, true));
        CHECK(WifiPolicy::apActive(true, false));
        CHECK(WifiPolicy::apActive(false, false));
        CHECK(!WifiPolicy::apActive(false, true));
        CHECK(WifiPolicy::retryDelay(0) == 1000 && WifiPolicy::retryDelay(99) == 30000);
        Aggregator agg;
        std::vector<Record> records;
        auto emit = [&](const Record &v) { records.push_back(v); };
        InverterState sample;
        sample.timestamp = 1704153595;
        sample.monotonicMs = 1000;
        sample.online = true;
        sample.channelCount = 2;
        sample.channels[0].power = 360;
        sample.channels[1].power = 360;
        sample.totalPower = 720;
        agg.add(sample, emit);
        sample.timestamp += 10;
        sample.monotonicMs += 10000;
        sample.energyDeltaWh = 2;
        agg.add(sample, emit);
        CHECK(near(agg.totals().totalWh, 2));
        CHECK(near(agg.totals().dayWh, 1));
        CHECK(agg.totals().day == 20240102);
        auto day = std::find_if(records.begin(), records.end(),
                                [](const Record &v) { return v.resolution == Resolution::Day; });
        CHECK(day != records.end() && near(day->energyWh, 1) && day->duration == 86400 &&
              day->day == 20240101);
        auto minute = std::find_if(records.begin(), records.end(),
                                   [](const Record &v) { return v.resolution == Resolution::Minute; });
        CHECK(minute != records.end() && minute->coverage == 5 && minute->channelCount == 2 &&
              near(minute->channels[0], 360));
        // UTC date restoration survives multi-day outages without assigning missing days zero production.
        Aggregator reboot;
        Record checkpoint = *minute;
        reboot.restore(checkpoint);
        records.clear();
        sample.timestamp = 1704326400;
        sample.energyDeltaWh = 0;
        reboot.add(sample, emit);
        CHECK(reboot.totals().totalWh == checkpoint.totalWh && reboot.totals().dayWh == 0);
        CHECK(records.size() == 1 && records[0].day == 20240101);
        StatisticsAccumulator stats(Totals{1500, 100, 500, 1704157200, 20240102}, 1704157200);
        Record daily;
        daily.resolution = Resolution::Day;
        daily.day = 20240101;
        daily.energyWh = 300;
        stats.add(daily);
        daily.day = 20231231;
        daily.energyWh = 700;
        stats.add(daily);
        daily.day = 20240103;
        daily.energyWh = 9000;
        stats.add(daily);
        auto summary = stats.result();
        CHECK(summary.todayWh == 100 && summary.yesterdayWh == 300 && summary.weekWh == 400);
        CHECK(summary.monthWh == 400 && summary.yearWh == 400 && summary.totalWh == 1500 &&
              summary.averageWh == 500);
        CHECK(summary.bestDay == 20231231 && summary.bestDayWh == 700 && summary.completedDays == 2);
        std::vector<uint8_t> pcap;
        auto bytes = [&](const uint8_t *p, size_t n) {
            pcap.insert(pcap.end(), p, p + n);
            return true;
        };
        CHECK(pcapngHeader(bytes));
        CapturedFrame frame;
        frame.channel = 16;
        frame.rssi = -48;
        frame.lqi = 230;
        frame.epochUs = 1704067200123456ULL;
        frame.length = poll.length;
        std::copy_n(poll.bytes.begin(), poll.length, frame.bytes.begin());
        CHECK(pcapngFrame(frame, bytes));
        std::ofstream out("tests/capture-test.pcapng", std::ios::binary);
        out.write(reinterpret_cast<const char *>(pcap.data()), pcap.size());
        out.close();
        std::cout << "PASS: " << checks
                  << " checks (energy, APsystems models, aggregation, serialization, NOR recovery, rings, radio frames, "
                     "passive TX gate, Wi-Fi policy, PCAPNG)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
