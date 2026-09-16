#include "protocol.hpp"
#include <algorithm>
namespace sol {
static uint16_t le(const uint8_t *p) {
    return p[0] | (uint16_t(p[1]) << 8);
}
Frame APSProtocol::envelope(uint16_t id, uint16_t dst, uint16_t cluster, const uint8_t *data, size_t n) {
    Frame f;
    if (n > 100)
        return f;
    auto add = [&](uint8_t v) { f.bytes[f.length++] = v; };
    auto word = [&](uint16_t v) {
        add(v);
        add(v >> 8);
    };
    word(dst == 0xffff ? 0x8841 : 0x8861);
    add(mac_++);
    word(id);
    word(dst);
    word(0);
    word(0x0008);
    word(dst);
    word(0);
    add(15);
    add(network_++);
    add(dst == 0xffff ? 0x08 : 0x00);
    add(0x14);
    word(cluster);
    word(0x0f05);
    add(0x14);
    add(aps_++);
    for (size_t i = 0; i < n; i++)
        add(data[i]);
    return f;
}
Frame APSProtocol::poll(uint16_t destination) {
    uint8_t p[19];
    std::reverse_copy(ecu.begin(), ecu.end(), p);
    const uint8_t cmd[] = {0xfb, 0xfb, 0x06, 0xbb, 0, 0, 0, 0, 0, 0, 0xc1, 0xfe, 0xfe};
    memcpy(p + 6, cmd, sizeof(cmd));
    return envelope(pan, destination, 0x0006, p, sizeof(p));
}
Frame APSProtocol::normal() {
    uint8_t p[30];
    std::reverse_copy(ecu.begin(), ecu.end(), p);
    const uint8_t cmd[] = {0xfb, 0xfb, 0x11, 0, 0, 0x0d, 0x60, 0x30, 0xfb, 0xd3, 0,    0,
                           0,    0,    0,    0, 0, 0,    4,    1,    2,    0x81, 0xfe, 0xfe};
    memcpy(p + 6, cmd, sizeof(cmd));
    return envelope(pan, 0xffff, 6, p, sizeof(p));
}
Frame APSProtocol::pair(unsigned step, const uint8_t serial[6]) {
    uint8_t p[17]{};
    uint16_t cluster = 0;
    size_t n = 0;
    if (step > 3)
        return {};
    if (step == 3) {
        cluster = 0x0101;
        std::reverse_copy(ecu.begin(), ecu.end(), p);
        n = 6;
    } else {
        memcpy(p, serial, 6);
        n = 6;
        if (step == 1)
            cluster = 0x020c;
        else {
            cluster = step == 0 ? 0x020d : 0x010f;
            p[6] = step == 0 ? 0xff : uint8_t(pan >> 8);
            p[7] = step == 0 ? 0xff : uint8_t(pan);
            p[8] = 0x10;
            p[9] = p[10] = 0xff;
            std::reverse_copy(ecu.begin(), ecu.end(), p + 11);
            n = 17;
        }
    }
    return envelope(0xffff, 0xffff, cluster, p, n);
}
Frame APSProtocol::routeRequest(uint16_t dst) {
    Frame f;
    auto add = [&](uint8_t v) { f.bytes[f.length++] = v; };
    auto word = [&](uint16_t v) {
        add(v);
        add(v >> 8);
    };
    word(0x8841);
    add(mac_++);
    word(pan);
    word(0xffff);
    word(0);
    word(0x1009);
    word(0xfffd);
    word(0);
    add(10);
    add(network_++);
    word(0xffff);
    for (auto i = ecu.rbegin(); i != ecu.rend(); ++i)
        add(*i);
    add(1);
    add(dst == 0xfffc ? 8 : 0);
    add(aps_++);
    word(dst);
    add(0);
    return f;
}
bool APSProtocol::parse(const uint8_t *p, size_t n, APSMessage &m) {
    m = {};
    if (n < 9)
        return false;
    uint16_t mac = le(p);
    // Reject encrypted MAC, sequence suppression, unsupported address modes/version.
    if ((mac & 7) != 1 || (mac & 0x0308) || ((mac >> 10) & 3) != 2 || ((mac >> 14) & 3) != 2 ||
        (mac & 0x3000))
        return false;
    m.pan = le(p + 3);
    size_t k = (mac & 0x40) ? 9 : 11;
    if (n < k + 8)
        return false;
    uint16_t nwk = le(p + k);
    if ((nwk & 0x0200) || ((nwk >> 2) & 15) != 2)
        return false;
    m.destination = le(p + k + 2);
    m.source = le(p + k + 4);
    m.sequence = p[k + 7];
    k += 8;
    if (nwk & 0x0800)
        k += 8;
    if (nwk & 0x1000)
        k += 8;
    if (nwk & 0x0400) {
        if (k + 2 > n)
            return false;
        size_t relays = p[k];
        k += 2 + 2 * relays;
    }
    if (k > n)
        return false;
    m.networkCommand = (nwk & 3) == 1;
    if (m.networkCommand) {
        m.payload = p + k;
        m.length = n - k;
        return true;
    }
    if ((nwk & 3) != 0 || k + 8 > n)
        return false;
    uint8_t aps = p[k];
    if ((aps & 3) != 0 || (aps & 0xe0) || (aps & 0x0c) == 0x0c)
        return false;
    m.endpoint = p[k + 1];
    m.cluster = le(p + k + 2);
    m.profile = le(p + k + 4);
    m.sequence = p[k + 7];
    k += 8;
    m.payload = p + k;
    m.length = n - k;
    return m.profile == 0x0f05 && m.endpoint == 0x14;
}
} // namespace sol
