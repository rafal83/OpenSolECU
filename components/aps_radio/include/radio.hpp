#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
class APSRadio {
  public:
    virtual ~APSRadio() = default;
    virtual bool begin() = 0;
    virtual bool send(const uint8_t *data, size_t length) = 0;
    virtual bool receive(uint8_t *buffer, size_t bufferSize, size_t *receivedLength, uint32_t timeoutMs) = 0;
};
class ESP32C6Radio final : public APSRadio {
  public:
    struct Packet {
        uint64_t monotonicUs;
        uint8_t bytes[125];
        uint8_t length;
        int8_t rssi;
        uint8_t lqi, channel;
    };
    QueueHandle_t rx = nullptr;
    SemaphoreHandle_t txDone = nullptr;
    volatile bool txOk = false;
    std::atomic<uint32_t> dropped{0}, txSubmissions{0}, txBlocked{0};
    bool passive = true;
    int rssi = 0, lqi = 0;
    uint8_t channel = 16;
    uint16_t pan = 0xa3d8;
    uint8_t extended[8] = {};
    bool begin() override;
    bool send(const uint8_t *, size_t) override;
    bool receive(uint8_t *, size_t, size_t *, uint32_t) override;
    bool receivePacket(Packet &packet, uint32_t timeoutMs);
    bool tune(uint8_t channel);

  private:
    uint8_t tx_[128]{};
};
