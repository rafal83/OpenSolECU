#include "web_server.hpp"
#include "api.hpp"
#include "web_assets.hpp"
#include <cstring>
namespace sol {
static esp_err_t asset(httpd_req_t *req) {
    const uint8_t *data = asset_html;
    size_t size = sizeof(asset_html);
    const char *type = "text/html; charset=utf-8";
    if (!strcmp(req->uri, "/app.js")) {
        data = asset_js;
        size = sizeof(asset_js);
        type = "text/javascript; charset=utf-8";
    } else if (!strcmp(req->uri, "/app.css")) {
        data = asset_css;
        size = sizeof(asset_css);
        type = "text/css; charset=utf-8";
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src "
                       "'self' data:; frame-ancestors 'none'; base-uri 'none'");
    return httpd_resp_send(req, reinterpret_cast<const char *>(data), size);
}
bool webBegin() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 8;
    config.send_wait_timeout = 4;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK)
        return false;
    apiRegister(server);
    httpd_uri_t page{};
    page.uri = "/*";
    page.method = HTTP_GET;
    page.handler = asset;
    return httpd_register_uri_handler(server, &page) == ESP_OK;
}
} // namespace sol
