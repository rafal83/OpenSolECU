idf_component_register(SRCS "updater.cpp" INCLUDE_DIRS "include" REQUIRES config logging esp_http_client esp_https_ota mbedtls json esp_app_format app_update)
