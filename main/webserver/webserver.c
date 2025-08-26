#include "webserver.h"
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>

static const char *TAG = "WebServer";
static uint32_t latest_distance = 0;
static httpd_handle_t server = NULL;

// HTTP GET handler for /distance
static esp_err_t distance_get_handler(httpd_req_t *req)
{
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"distance\": %u}\n", (unsigned int)latest_distance);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webserver_publish_distance(uint32_t distance)
{
    latest_distance = distance;
}

esp_err_t webserver_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_uri_t distance_uri = {
        .uri = "/distance",
        .method = HTTP_GET,
        .handler = distance_get_handler,
        .user_ctx = NULL};

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }
    httpd_register_uri_handler(server, &distance_uri);
    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
