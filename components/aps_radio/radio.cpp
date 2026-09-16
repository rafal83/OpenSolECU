#include "radio.hpp"
#include "esp_attr.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "logging.hpp"
#include <cstring>
static ESP32C6Radio *active = nullptr;
bool ESP32C6Radio::begin() {
#ifdef CONFIG_OPENSOLECU_FORCE_SNIFFER
    passive = true;
#endif
    rx = xQueueCreate(24, sizeof(Packet));
    txDone = xSemaphoreCreateBinary();
    if (!rx || !txDone)
        return false;
    active = this;
    if (esp_ieee802154_enable() != ESP_OK)
        return false;
    if (passive) {
        // IDF 5.5.1 set_promiscuous(true) disables auto ACK RX, auto ACK TX and enhanced ACK TX.
        // It is configured before RX starts. No PAN/coordinator/network configuration in this branch.
        return esp_ieee802154_set_promiscuous(true) == ESP_OK &&
               esp_ieee802154_set_channel(channel) == ESP_OK &&
               esp_ieee802154_set_rx_when_idle(true) == ESP_OK && esp_ieee802154_receive() == ESP_OK;
    }
    return esp_ieee802154_set_promiscuous(false) == ESP_OK && esp_ieee802154_set_channel(channel) == ESP_OK &&
           esp_ieee802154_set_panid(pan) == ESP_OK && esp_ieee802154_set_short_address(0) == ESP_OK &&
           esp_ieee802154_set_extended_address(extended) == ESP_OK &&
           esp_ieee802154_set_coordinator(true) == ESP_OK &&
           esp_ieee802154_set_rx_when_idle(true) == ESP_OK && esp_ieee802154_receive() == ESP_OK;
}
bool ESP32C6Radio::send(const uint8_t *p, size_t n) {
#ifdef CONFIG_OPENSOLECU_FORCE_SNIFFER
    ++txBlocked;
    sol::log(0, "ERROR: TX attempted while passive sniffer is active");
    return false;
#else
    if (passive || sol::configGet().sniffer) {
        ++txBlocked;
        sol::log(0, "ERROR: TX attempted while passive sniffer is active");
        return false;
    }
    if (!active || n > 125 || n < 3)
        return false;
    xSemaphoreTake(txDone, 0);
    txOk = false;
    tx_[0] = n + 2;
    memcpy(tx_ + 1, p, n);
    if (!sol::TransmitPolicy::send(passive || sol::configGet().sniffer, [&]() {
            ++txSubmissions;
            return esp_ieee802154_transmit(tx_, true) == ESP_OK;
        }))
        return false;
    // Keep tx_ alive even when caller times out; sleep cancels the hardware operation.
    if (xSemaphoreTake(txDone, pdMS_TO_TICKS(250)) != pdTRUE) {
        esp_ieee802154_sleep();
        esp_ieee802154_receive();
        return false;
    }
    return txOk;
#endif
}
bool ESP32C6Radio::receive(uint8_t *p, size_t cap, size_t *n, uint32_t timeout) {
    if (!n)
        return false;
    *n = 0;
    Packet v;
    if (xQueueReceive(rx, &v, pdMS_TO_TICKS(timeout)) != pdTRUE || v.length > cap)
        return false;
    memcpy(p, v.bytes, v.length);
    *n = v.length;
    rssi = v.rssi;
    lqi = v.lqi;
    return true;
}
bool ESP32C6Radio::receivePacket(Packet &p, uint32_t timeout) {
    return xQueueReceive(rx, &p, pdMS_TO_TICKS(timeout)) == pdTRUE;
}
bool ESP32C6Radio::tune(uint8_t c) {
    if (!passive || c < 11 || c > 26)
        return false;
    esp_ieee802154_sleep();
    xQueueReset(rx);
    channel = c;
    return esp_ieee802154_set_channel(c) == ESP_OK && esp_ieee802154_receive() == ESP_OK;
}
extern "C" void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info) {
    BaseType_t wake = pdFALSE;
    if (active && frame[0] >= 2 && frame[0] <= 127) {
        ESP32C6Radio::Packet p{};
        p.monotonicUs = esp_timer_get_time();
        p.channel = info->channel;
        p.length = frame[0] - 2;
        memcpy(p.bytes, frame + 1, p.length);
        p.rssi = info->rssi;
        p.lqi = info->lqi;
        if (xQueueSendFromISR(active->rx, &p, &wake) != pdTRUE)
            active->dropped++;
    }
    esp_ieee802154_receive_handle_done(frame);
    if (wake)
        portYIELD_FROM_ISR();
}
extern "C" void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *, const uint8_t *ack,
                                                       esp_ieee802154_frame_info_t *) {
    BaseType_t wake = pdFALSE;
    if (ack)
        esp_ieee802154_receive_handle_done(ack);
    if (active) {
        active->txOk = true;
        xSemaphoreGiveFromISR(active->txDone, &wake);
    }
    if (wake)
        portYIELD_FROM_ISR();
}
extern "C" void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *, esp_ieee802154_tx_error_t) {
    BaseType_t wake = pdFALSE;
    if (active) {
        active->txOk = false;
        xSemaphoreGiveFromISR(active->txDone, &wake);
    }
    if (wake)
        portYIELD_FROM_ISR();
}
