#include "time_manager.hpp"
#include "config.hpp"
#include "esp_netif_sntp.h"
#include <cstdlib>
namespace sol {
void timeBegin() {
    auto c = configGet();
    const char *tz = !strcmp(c.timezone, "Europe/Paris") ? "CET-1CEST,M3.5.0,M10.5.0/3"
                     : !strcmp(c.timezone, "UTC")        ? "UTC0"
                                                         : c.timezone + 6;
    setenv("TZ", tz, 1);
    tzset();
    esp_sntp_config_t sn = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sn));
}
bool timeValid() {
    return time(nullptr) >= 1704067200;
}
} // namespace sol
