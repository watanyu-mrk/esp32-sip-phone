#include "web_server.h"

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "web_internal.h"

static const char *TAG = "WEB";

static httpd_handle_t s_server;

/* Embedded copies of web/ (see EMBED_TXTFILES in CMakeLists.txt). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");

/* Static assets hold no secrets and are served without auth so the UI still
 * boots when a browser does not replay Basic credentials for subresources.
 * The HTML shell and every /api route stay protected. */
static esp_err_t send_asset(httpd_req_t *req, const uint8_t *start, const uint8_t *end,
                            const char *content_type, const char *cache_control)
{
    /* EMBED_TXTFILES appends a NUL terminator that _end points past; sending it
     * makes browsers reject app.js with a parse error. Send only the text. */
    size_t len = strnlen((const char *)start, (size_t)(end - start));

    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    return httpd_resp_send(req, (const char *)start, (ssize_t)len);
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (!web_auth_check(req)) {
        return ESP_OK;
    }
    return send_asset(req, index_html_start, index_html_end, "text/html; charset=utf-8",
                      "no-store");
}

static esp_err_t css_get_handler(httpd_req_t *req)
{
    return send_asset(req, style_css_start, style_css_end, "text/css", "no-store");
}

static esp_err_t js_get_handler(httpd_req_t *req)
{
    return send_asset(req, app_js_start, app_js_end, "application/javascript", "no-store");
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WEB] httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL },
        { .uri = "/index.html", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL },
        { .uri = "/style.css", .method = HTTP_GET, .handler = css_get_handler, .user_ctx = NULL },
        { .uri = "/app.js", .method = HTTP_GET, .handler = js_get_handler, .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(s_server, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[WEB] Failed to register %s: %s", routes[i].uri,
                     esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }

    err = web_api_init(s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[WEB] Failed to register API routes: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "[WEB] HTTP server started on port 80");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
