
#include "webserver.h"
#include "wifi_setup.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>

// HTTP GET handler for /
static esp_err_t index_get_handler(httpd_req_t *req)
{
    char html[2048];
    const char *ssid = wifi_get_ssid();
    snprintf(html, sizeof(html),
             "<!DOCTYPE html>"
             "<html><head><title>FloraLink.Hub</title>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<style>"
             "body{font-family:sans-serif;background:#f4f8fb;margin:0;padding:0;}"
             ".container{max-width:400px;margin:40px auto 0 auto;background:#fff;border-radius:12px;box-shadow:0 2px 8px #0001;padding:32px 24px 24px 24px;}"
             "h1{margin-top:0;font-size:2em;color:#2196f3;}"
             "#distance{font-size:2.5em;color:#2196f3;margin:16px 0;}"
             "#error{color:#f44336;margin-bottom:16px;}"
             "footer{margin-top:32px;font-size:0.95em;color:#888;}"
             "</style>"
             "</head><body>"
             "<div class='container'>"
             "<h1>FloraLink.Hub</h1>"
             "<div>Current Distance:</div>"
             "<div id='distance'>--</div>"
             "<div id='error'></div>"
             "<footer id='footer'></footer>"
             "</div>"
             "<script>"
             "const ssid = '%s';"
             "function fetchDistance(){"
             "fetch('/distance').then(r=>r.json()).then(j=>{"
             "  document.getElementById('distance').textContent = j.distance + ' cm';"
             "  if(j.error && j.error !== 0){document.getElementById('error').textContent = 'Error: 0x' + j.error.toString(16).toUpperCase();}"
             "  else{document.getElementById('error').textContent = '';}"
             "});"
             "}"
             "function updateFooter(){"
             "  const now = new Date();"
             "  const date = now.toLocaleDateString();"
             "  const time = now.toLocaleTimeString();"
             "  document.getElementById('footer').textContent = 'On WLAN: ' + ssid + ', ' + date + ', ' + time;"
             "}"
             "fetchDistance();setInterval(fetchDistance,1000);"
             "updateFooter();setInterval(updateFooter,1000);"
             "</script>"
             "</body></html>",
             ssid);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const char *TAG = "WebServer";
static uint32_t latest_distance = 0;
static int32_t latest_error = 0;
static httpd_handle_t server = NULL;

// HTTP GET handler for /distance
static esp_err_t distance_get_handler(httpd_req_t *req)
{
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"distance\": %u, \"error\": %d}\n", (unsigned int)latest_distance, (int)latest_error);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webserver_publish_distance(uint32_t distance)
{
    latest_distance = distance;
    latest_error = 0;
}

void webserver_publish_error(int32_t error_code)
{
    latest_distance = 0;
    latest_error = error_code;
}

esp_err_t webserver_init(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    // Register root HTML page
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL};

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
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &distance_uri);
    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
