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
// HTTP GET handler for /stats
static esp_err_t stats_get_handler(httpd_req_t *req)
{
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

/* HTTP GET handler for /configure */
static esp_err_t configure_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>Configure</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
    SEND_HTML_CHUNK("<style>"
                    "body{font-family:sans-serif;background:#f4f8fb;}"
                    ".container{max-width:400px;min-height:480px;margin:40px auto 0 auto;background:#fff;border-radius:12px;box-shadow:0 2px 8px #0001;padding:0 0 24px 0;}"
                    ".nav{display:flex;gap:0;padding:0 24px 0 24px;border-radius:12px 12px 0 0;background:#fff;box-shadow:0 2px 8px #0001;overflow:hidden;position:relative;}"
                    ".nav a{flex:1;text-align:center;padding:6px 0;font-weight:500;text-decoration:none;color:#2196f3;background:#e3eaf3;border:none;transition:background 0.2s,color 0.2s;font-size:0.98em;position:relative;z-index:1;}"
                    ".nav a.active{background:#fff;color:#1565c0;cursor:default;}"
                    ".nav a:not(.active):hover{background:#d0e2fa;}"
                    ".nav .tab-underline{position:absolute;bottom:0;left:0;height:2px;width:50%;background:#2196f3;transition:left 0.3s cubic-bezier(.4,0,.2,1),width 0.3s cubic-bezier(.4,0,.2,1);z-index:2;}"
                    "h2{margin:16px 24px 0 24px;color:#2196f3;}"
                    "form{margin:24px 24px 0 24px;}"
                    "label{display:block;margin:18px 0 6px;}"
                    "input[type=number]{width:100%%;padding:8px;font-size:1em;}"
                    "button{margin-top:18px;padding:8px 16px;font-size:1em;border:none;border-radius:6px;background:#2196f3;color:#fff;cursor:pointer;}"
                    "</style>");
    SEND_HTML_CHUNK("<script>function moveTabUnderline(){var nav=document.querySelector('.nav');if(!nav)return;var active=nav.querySelector('.active');var underline=nav.querySelector('.tab-underline');if(active&&underline){underline.style.left=active.offsetLeft+'px';underline.style.width=active.offsetWidth+'px';}}window.addEventListener('DOMContentLoaded',moveTabUnderline);window.addEventListener('resize',moveTabUnderline);</script>");
    SEND_HTML_CHUNK("</head><body><div class='container'>");
    SEND_HTML_CHUNK("<nav class='nav'><a href='/' >Home</a><a href='/configure' class='active'>Configure</a><div class='tab-underline'></div></nav>");
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
static esp_err_t index_get_handler(httpd_req_t *req)
{
    const char *ssid = wifi_get_ssid();
    httpd_resp_set_type(req, "text/html");

    // Send static HTML in flash-resident chunks (saves needing a 4KB RAM buffer)
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><title>FloraLink.Hub</title>");
    SEND_HTML_CHUNK("<meta name='viewport' content='width=device-width,initial-scale=1'>");
    SEND_HTML_CHUNK(
        "<style>"
        "body{font-family:sans-serif;background:#f4f8fb;margin:0;padding:0;}"
        ".container{max-width:400px;min-height:480px;margin:40px auto 0 auto;background:#fff;border-radius:12px;box-shadow:0 2px 8px #0001;padding:0 0 24px 0;}"
        ".nav{display:flex;gap:0;padding:0 24px 0 24px;border-radius:12px 12px 0 0;background:#fff;box-shadow:0 2px 8px #0001;overflow:hidden;position:relative;}"
        ".nav a{flex:1;text-align:center;padding:6px 0;font-weight:500;text-decoration:none;color:#2196f3;background:#e3eaf3;border:none;transition:background 0.2s,color 0.2s;font-size:0.98em;position:relative;z-index:1;}"
        ".nav a.active{background:#fff;color:#1565c0;cursor:default;}"
        ".nav a:not(.active):hover{background:#d0e2fa;}"
        ".nav .tab-underline{position:absolute;bottom:0;left:0;height:2px;width:50%%;background:#2196f3;transition:left 0.3s cubic-bezier(.4,0,.2,1),width 0.3s cubic-bezier(.4,0,.2,1);z-index:2;}"
        "h1{margin:24px 24px 0 24px;font-size:2em;color:#2196f3;}"
        ".distance-label{margin:16px 0 0 24px;font-size:1.1em;}"
        "#distance{font-size:2.5em;color:#2196f3;margin:8px 0 0 24px;}"
        "#error{color:#f44336;margin:0 0 16px 24px;}"
        "footer{margin:32px 0 0 24px;font-size:0.95em;color:#888;}"
        "#statsBtn{margin:20px 0 0 24px;padding:8px 16px;font-size:1em;border:none;border-radius:6px;background:#2196f3;color:#fff;cursor:pointer;}"
        "#statsPanel{display:none;margin:16px 24px 0 24px;padding:12px;background:#f0f4fa;border-radius:8px;font-size:1em;}"
        "#statsPanel table{width:100%%;border-collapse:collapse;}"
        "#statsPanel td{padding:4px 8px;}"
        "</style>"
        "<script>function moveTabUnderline(){var nav=document.querySelector('.nav');if(!nav)return;var active=nav.querySelector('.active');var underline=nav.querySelector('.tab-underline');if(active&&underline){underline.style.left=active.offsetLeft+'px';underline.style.width=active.offsetWidth+'px';}}window.addEventListener('DOMContentLoaded',moveTabUnderline);window.addEventListener('resize',moveTabUnderline);</script>"
        "</head><body><div class='container'>"
        "<nav class='nav'><a href='/' class='active'>Home</a><a href='/configure'>Configure</a><div class='tab-underline'></div></nav>"
        "<h1>FloraLink.Hub</h1><div class='distance-label'>Current Distance:</div>"
        "<div id='distance'>--</div><div id='error'></div>"
        "<button id='statsBtn' onclick='toggleStats()'>Show Device Stats</button>"
        "<div id='statsPanel'><table>"
        "<tr><td>Free Heap:</td><td id='statHeap'>-</td></tr>"
        "<tr><td>Min Heap:</td><td id='statMinHeap'>-</td></tr>"
        "<tr><td>Uptime:</td><td id='statUptime'>-</td></tr>"
        "<tr><td>CPU Load:</td><td id='statCpuLoad'>-</td></tr>"
        "</table></div><footer id='footer'></footer></div><script>");

    // Dynamic SSID JS snippet (small stack buffer only)
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
    int n = snprintf(dyn, sizeof(dyn), "const ssid='%s';", esc);
    if (n < 0 || n >= (int)sizeof(dyn))
    {
        ESP_LOGW("WebServer", "SSID JS snippet truncated");
    }
    /* Dynamic SSID JS snippet */
    SEND_HTML_CHUNK(dyn);

    /* Dynamic SSID JS snippet */
    SEND_HTML_CHUNK(
        "function fetchDistance(){fetch('/distance').then(r=>r.json()).then(j=>{"
        "document.getElementById('distance').textContent=j.distance+' cm';"
        "if(j.error&&j.error!==0){document.getElementById('error').textContent='Error: 0x'+j.error.toString(16).toUpperCase();}"
        "else{document.getElementById('error').textContent='';}});}"
        "function updateFooter(){const now=new Date();const date=now.toLocaleDateString();const time=now.toLocaleTimeString();"
        "document.getElementById('footer').textContent='On WLAN: '+ssid+', '+date+', '+time;}"
        "let statsVisible=false;let statsInterval=null;"
        "function toggleStats(){statsVisible=!statsVisible;document.getElementById('statsPanel').style.display=statsVisible?'block':'none';"
        "document.getElementById('statsBtn').textContent=statsVisible?'Hide Device Stats':'Show Device Stats';"
        "if(statsVisible){fetchStats();statsInterval=setInterval(fetchStats,1000);}else{if(statsInterval)clearInterval(statsInterval);statsInterval=null;}}"
        "function fetchStats(){fetch('/stats').then(r=>r.json()).then(j=>{"
        "var freeKB = (typeof j.free_heap === 'number') ? Math.round(j.free_heap/1024) : '-';"
        "var minFreeKB = (typeof j.min_free_heap === 'number') ? Math.round(j.min_free_heap/1024) : '-';"
        "document.getElementById('statHeap').textContent = freeKB + ' KB';"
        "document.getElementById('statMinHeap').textContent = minFreeKB + ' KB';"
        "let ms=j.uptime_ms;let sec=Math.floor(ms/1000)%60,min=Math.floor(ms/60000)%60,hr=Math.floor(ms/3600000);"
        "document.getElementById('statUptime').textContent=hr+'h '+min+'m '+sec+'s';"
        "document.getElementById('statCpuLoad').textContent=Math.round(j.cpu_load*100)+'%';});}"
        "fetchDistance();setInterval(fetchDistance,1000);updateFooter();setInterval(updateFooter,1000);"
        "</script></body></html>");

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

    esp_err_t ret = httpd_start(&server, &config_http);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s, code: 0x%X", esp_err_to_name(ret), ret);
        return ret;
    }
    /* Register URI handlers */
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &distance_uri);
    // Register /stats endpoint
    httpd_uri_t stats_uri = {
        .uri = "/stats",
        .method = HTTP_GET,
        .handler = stats_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &stats_uri);
    // Register /configure GET and POST
    httpd_uri_t configure_get_uri = {
        .uri = "/configure",
        .method = HTTP_GET,
        .handler = configure_get_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &configure_get_uri);
    httpd_uri_t configure_post_uri = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = configure_post_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &configure_post_uri);

    // websocket server init and check for errors
    bool ws_ret = websockserver_init(server);
    if (!ws_ret)
    {
        ESP_LOGE(TAG, "Failed to start WebSocket server");
    }
    ESP_LOGI(TAG, "Web server started on port %d", config_http.server_port);
    return ret;
}