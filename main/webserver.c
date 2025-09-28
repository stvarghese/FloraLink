#include "modemanager.h"
#include "blink_config.h"

#include "webserver.h"
#include "websockserver.h"
#include "wifi_setup.h"
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include "monitor.h"
#include "nodeio.h"

extern const unsigned char webpage_main_css_start[] asm("_binary_main_css_start");
extern const unsigned char webpage_main_css_end[] asm("_binary_main_css_end");
extern const unsigned char webpage_main_js_start[] asm("_binary_main_js_start");
extern const unsigned char webpage_main_js_end[] asm("_binary_main_js_end");

// HTTP GET handler for /nodeslist
static esp_err_t nodeslist_get_handler(httpd_req_t *req)
{
    char *json = (char *)malloc(2048);
    if (!json)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    nodeio_publish_nodeslist(json, 2048);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ESP_OK;
}

// Helper macro to send a chunk and log any error; on error, return immediately
#define SEND_HTML_CHUNK(str_literal)                                               \
    do                                                                             \
    {                                                                              \
        esp_err_t __e = httpd_resp_sendstr_chunk(req, (str_literal));              \
        if (__e != ESP_OK)                                                         \
        {                                                                          \
            ESP_LOGE("WebServer", "chunk send failed (%s)", esp_err_to_name(__e)); \
            return __e;                                                            \
        }                                                                          \
    } while (0)

// HTTP GET handler for /stats
static esp_err_t stats_get_handler(httpd_req_t *req)
{
    // If browser requests with Accept: text/html, serve a simple stats page
    char accept_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept", accept_hdr, sizeof(accept_hdr)) == ESP_OK && strstr(accept_hdr, "text/html"))
    {
        httpd_resp_set_type(req, "text/html");
        SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>Device Stats</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
        SEND_HTML_CHUNK("<link rel='stylesheet' href='/main.css'>");
        SEND_HTML_CHUNK("<script src='/main.js'></script>");
        SEND_HTML_CHUNK("</head><body><div class='container'>");
        SEND_HTML_CHUNK("    <nav class='nav'>\n"
                        "        <a href='/'>Home</a>\n"
                        "        <a href='/configure'>Configure</a>\n"
                        "        <a href='/nodes'>Nodes</a>\n"
                        "        <a href='/stats' class='active'>Stats</a>\n"
                        "        <div class='tab-underline'></div>\n"
                        "    </nav>");
        SEND_HTML_CHUNK("<h2>Device Stats</h2>");
        SEND_HTML_CHUNK("<div id='statsPanel'><table>");
        SEND_HTML_CHUNK("<tr><td>Free Heap:</td><td id='statHeap'>-</td></tr>");
        SEND_HTML_CHUNK("<tr><td>Min Heap:</td><td id='statMinHeap'>-</td></tr>");
        SEND_HTML_CHUNK("<tr><td>Uptime:</td><td id='statUptime'>-</td></tr>");
        SEND_HTML_CHUNK("<tr><td>CPU Load:</td><td id='statCpuLoad'>-</td></tr>");
        SEND_HTML_CHUNK("</table></div></div></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    // Otherwise, serve JSON
    device_stats_t stats;
    monitor_get_device_stats(&stats);
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"free_heap\":%u,\"min_free_heap\":%u,\"uptime_ms\":%llu,\"cpu_load\":%.2f}\n",
             (unsigned int)stats.free_heap,
             (unsigned int)stats.min_free_heap,
             (unsigned long long)stats.uptime_ms,
             stats.cpu_load);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* HTTP GET handler for /configure */
static esp_err_t configure_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>Configure</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
    SEND_HTML_CHUNK("<link rel='stylesheet' href='/main.css'>");
    SEND_HTML_CHUNK("<script src='/main.js'></script>");
    SEND_HTML_CHUNK("</head><body><div class='container'>");
    SEND_HTML_CHUNK("    <nav class='nav'>\n"
                    "        <a href='/'>Home</a>\n"
                    "        <a href='/configure' class='active'>Configure</a>\n"
                    "        <a href='/nodes'>Nodes</a>\n"
                    "        <div class='tab-underline'></div>\n"
                    "    </nav>");
    SEND_HTML_CHUNK("<h2>Configure LED Blink</h2>");
    SEND_HTML_CHUNK("<form method='POST' action='/configure'>");
    SEND_HTML_CHUNK("<label for='period'>Blink Period (ms):</label>");
    char input[128];
    snprintf(input, sizeof(input), "<input type='number' id='period' name='period' min='%d' max='%d' value='%lu' required>", BLINK_PERIOD_MIN, BLINK_PERIOD_MAX, blink_get_period_ms());
    SEND_HTML_CHUNK(input);
    SEND_HTML_CHUNK("<button type='submit'>Update</button>");
    SEND_HTML_CHUNK("</form>");
    // Sleep/Deep Sleep buttons
    SEND_HTML_CHUNK("<form method='POST' action='/configure' style='margin-top:32px;display:flex;gap:16px;justify-content:center;'>");
    SEND_HTML_CHUNK("<button name='sleep' value='light' type='submit' style='background:#ffb300;color:#fff;'>Sleep</button>");
    SEND_HTML_CHUNK("<button name='sleep' value='deep' type='submit' style='background:#d32f2f;color:#fff;'>Deep Sleep</button>");
    SEND_HTML_CHUNK("</form>");
    SEND_HTML_CHUNK("</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL); // End chunked response
    return ESP_OK;
}

// HTTP GET handler for /nodes
static esp_err_t nodes_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>Nodes</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
    SEND_HTML_CHUNK("<link rel='stylesheet' href='/main.css'>");
    SEND_HTML_CHUNK("<script src='/main.js'></script>");
    SEND_HTML_CHUNK("</head><body><div class='container'>");
    SEND_HTML_CHUNK("    <nav class='nav'>\n"
                    "        <a href='/'>Home</a>\n"
                    "        <a href='/configure'>Configure</a>\n"
                    "        <a href='/nodes' class='active'>Nodes</a>\n"
                    "        <div class='tab-underline'></div>\n"
                    "    </nav>");
    SEND_HTML_CHUNK("<h2>Connected Nodes</h2>");
    SEND_HTML_CHUNK("<div id='nodesPanel'>Loading...</div>");
    SEND_HTML_CHUNK("</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* HTTP POST handler for /configure */
static esp_err_t configure_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    // Parse: look for sleep/deep sleep or period
    if (strstr(buf, "sleep=light"))
    {
        modemanager_light_sleep();
    }
    else if (strstr(buf, "sleep=deep"))
    {
        modemanager_deep_sleep();
    }
    else
    {
        char *p = strstr(buf, "period=");
        uint32_t period = 0;
        if (p)
        {
            period = (uint32_t)atoi(p + 7);
            blink_set_period_ms(period);
        }
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><script>window.location='/configure';</script></body></html>");
    return ESP_OK;
}
// HTTP GET handler for /
// Handler to serve main.css
static esp_err_t css_get_handler(httpd_req_t *req)
{
    size_t css_len = webpage_main_css_end - webpage_main_css_start;
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)webpage_main_css_start, css_len);
    return ESP_OK;
}

// Handler to serve main.js
static esp_err_t js_get_handler(httpd_req_t *req)
{
    size_t js_len = webpage_main_js_end - webpage_main_js_start;
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)webpage_main_js_start, js_len);
    return ESP_OK;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    const char *ssid = wifi_get_ssid();
    ESP_LOGI("WebServer", "SSID for HTML injection: '%s'", ssid ? ssid : "(null)");
    httpd_resp_set_type(req, "text/html");

    // Send static HTML in flash-resident chunks (saves needing a 4KB RAM buffer)
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>FloraLink.Hub</title>");
    SEND_HTML_CHUNK("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    SEND_HTML_CHUNK("<link rel='stylesheet' href='/main.css'>");
    // Inject SSID JS variable before main.js
    char dyn[128];
    char esc[96];
    size_t ei = 0;
    for (size_t i = 0; ssid && ssid[i] && ei < sizeof(esc) - 2; ++i)
    {
        if (ssid[i] == '\'')
            esc[ei++] = '\\';
        esc[ei++] = ssid[i];
    }
    esc[ei] = '\0';
    int n = snprintf(dyn, sizeof(dyn), "<script>const ssid='%s';</script>", esc);
    if (n < 0 || n >= (int)sizeof(dyn))
    {
        ESP_LOGW("WebServer", "SSID JS snippet truncated");
    }
    SEND_HTML_CHUNK(dyn);
    SEND_HTML_CHUNK("<script src='/main.js'></script>");
    SEND_HTML_CHUNK("</head><body><div class='container'>");
    SEND_HTML_CHUNK("    <nav class='nav'>\n"
                    "        <a href='/' class='active'>Home</a>\n"
                    "        <a href='/configure'>Configure</a>\n"
                    "        <a href='/nodes'>Nodes</a>\n"
                    "        <div class='tab-underline'></div>\n"
                    "    </nav>");
    SEND_HTML_CHUNK("<h1>FloraLink.Hub</h1><div class='distance-label'>Current Distance:</div>");
    SEND_HTML_CHUNK("<div id='distance'>--</div><div id='error'></div>");
    SEND_HTML_CHUNK("<button id='statsBtn' onclick='toggleStats()'>Show Device Stats</button>");
    SEND_HTML_CHUNK("<div id='statsPanel'><table>");
    SEND_HTML_CHUNK("<tr><td>Free Heap:</td><td id='statHeap'>-</td></tr>");
    SEND_HTML_CHUNK("<tr><td>Min Heap:</td><td id='statMinHeap'>-</td></tr>");
    SEND_HTML_CHUNK("<tr><td>Uptime:</td><td id='statUptime'>-</td></tr>");
    SEND_HTML_CHUNK("<tr><td>CPU Load:</td><td id='statCpuLoad'>-</td></tr>");
    SEND_HTML_CHUNK("</table></div><footer id='footer'></footer></div>");
    SEND_HTML_CHUNK("</body></html>");

    esp_err_t te = httpd_resp_send_chunk(req, NULL, 0);
    if (te != ESP_OK)
    {
        ESP_LOGE("WebServer", "final chunk termination failed (%s)", esp_err_to_name(te));
        return te;
    }
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

/* Web server initialization */
esp_err_t webserver_init(void)
{
    // Define HTTP server configuration
    httpd_config_t config_http = HTTPD_DEFAULT_CONFIG();
    config_http.server_port = 80;
    // increase number of uri hanlers
    config_http.max_uri_handlers = 12;

    // --- Define URI handlers ---
    // HTML pages
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL};
    httpd_uri_t nodes_uri = {
        .uri = "/nodes",
        .method = HTTP_GET,
        .handler = nodes_get_handler,
        .user_ctx = NULL};
    httpd_uri_t stats_uri = {
        .uri = "/stats",
        .method = HTTP_GET,
        .handler = stats_get_handler,
        .user_ctx = NULL};
    httpd_uri_t configure_get_uri = {
        .uri = "/configure",
        .method = HTTP_GET,
        .handler = configure_get_handler,
        .user_ctx = NULL};

    // API endpoints (JSON/data)
    httpd_uri_t distance_uri = {
        .uri = "/distance",
        .method = HTTP_GET,
        .handler = distance_get_handler,
        .user_ctx = NULL};
    httpd_uri_t configure_post_uri = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = configure_post_handler,
        .user_ctx = NULL};
    httpd_uri_t nodeslist_uri = {
        .uri = "/nodeslist",
        .method = HTTP_GET,
        .handler = nodeslist_get_handler,
        .user_ctx = NULL};

    // Static assets
    httpd_uri_t css_uri = {
        .uri = "/main.css",
        .method = HTTP_GET,
        .handler = css_get_handler,
        .user_ctx = NULL};
    httpd_uri_t js_uri = {
        .uri = "/main.js",
        .method = HTTP_GET,
        .handler = js_get_handler,
        .user_ctx = NULL};

    // --- Start HTTP server ---
    esp_err_t ret = httpd_start(&server, &config_http);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s, code: 0x%X", esp_err_to_name(ret), ret);
        return ret;
    }

    // --- Register URI handlers in logical groups ---
    // HTML pages
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &nodes_uri);
    httpd_register_uri_handler(server, &stats_uri);
    httpd_register_uri_handler(server, &configure_get_uri);

    // API endpoints
    httpd_register_uri_handler(server, &distance_uri);
    httpd_register_uri_handler(server, &configure_post_uri);
    httpd_register_uri_handler(server, &nodeslist_uri);

    // Static assets
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);

    // --- WebSocket server ---
    bool ws_ret = websockserver_init(server);
    if (!ws_ret)
    {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
    }
    ESP_LOGI(TAG, "Web server started on port %d", config_http.server_port);
    return ret;
}