#pragma once
#include "sol.hpp"
namespace sol {
struct Frame {
    std::array<uint8_t, 125> bytes{};
    size_t length = 0;
};
struct APSMessage {
    uint16_t pan = 0, source = 0, destination = 0, cluster = 0, profile = 0;
    uint8_t endpoint = 0, sequence = 0;
    const uint8_t *payload = nullptr;
    size_t length = 0;
    bool networkCommand = false;
};
class APSProtocol {
    uint8_t mac_ = 0, network_ = 0, aps_ = 0;
    Frame envelope(uint16_t pan, uint16_t destination, uint16_t cluster, const uint8_t *data, size_t n);

  public:
    uint16_t pan = 0xa3d8;
    std::array<uint8_t, 6> ecu = {0xd8, 0xa3, 0x01, 0x1b, 0x97, 0x80};
    Frame poll(uint16_t destination);
    Frame normal();
    Frame pair(unsigned step, const uint8_t serial[6]);
    Frame routeRequest(uint16_t destination);
    static bool parse(const uint8_t *p, size_t n, APSMessage &msg);
};
} // namespace sol
