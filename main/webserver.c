#include "modemanager.h"
#include "onboardled.h"

#include "webserver.h"
#include "websockserver.h"
#include "wifi_setup.h"
#include "commonutils.h"
#include "secure_boot_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include "monitor.h"
#include "nodeio.h"
#include <esp_timer.h>
#include "cJSON.h"
#include "nodeio_lut.h"
#include "nodeio_sensors.h"
#include "nodeio_services.h"
#include "nvm.h"
#include "appl_nvm.h"
// #include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const unsigned char webpage_main_css_start[] asm("_binary_main_css_start");
extern const unsigned char webpage_main_css_end[] asm("_binary_main_css_end");
extern const unsigned char webpage_main_js_start[] asm("_binary_main_js_start");
extern const unsigned char webpage_main_js_end[] asm("_binary_main_js_end");

// Global flag for light sleep mode
volatile int g_is_light_sleep = 0;

// HTTP GET handler for /nodeslist
static esp_err_t nodeslist_get_handler(httpd_req_t *req)
{
    /* Any HTTP request normally signifies user activity and extends the ACTIVE window.
       To allow passive polling (for example by automated health checks) without
       waking the system, a client may send the header `X-No-Extend: 1` to
       suppress the automatic active-window notification. Default behaviour
       remains unchanged (no header -> wake). */
    char noextend_hdr[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-No-Extend", noextend_hdr, sizeof(noextend_hdr)) != ESP_OK || strcmp(noextend_hdr, "1") != 0)
    {
        modemanager_notify_activity_auto();
    }
    HEAP_TRACE_START("NODESLIST");

    // Allocate a larger buffer for nodeslist JSON; 4K should be enough for typical node counts
    char *json = (char *)malloc(4096);
    if (!json)
    {
        httpd_resp_send_500(req);
        HEAP_TRACE_END_DEFAULT();
        return ESP_FAIL;
    }
    size_t len = nodeio_publish_nodeslist(json, 4096);
    if (len == 0)
    {
        ESP_LOGW("WebServer", "nodeslist generation returned zero length");
    }
    // If the generated JSON filled the buffer, warn about possible truncation
    if (len >= 4095)
    {
        ESP_LOGW("WebServer", "nodeslist JSON possibly truncated (len=%u, buf=4096)", (unsigned)len);
    }
    // Explicitly instruct the client this connection will be closed
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    free(json);

    /* Runtime diagnostic: report current task stack high-water mark (words). */
    ESP_LOGD("WebServer", "nodeslist handler stack high-water (words): %u", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    HEAP_TRACE_END(100); // Higher threshold for HTTP response processing
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

// `url_decode` is provided by commonutils.h (returns int). Do not define a
// conflicting static helper here.

// HTTP GET handler for /stats
static esp_err_t stats_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    // If browser requests with Accept: text/html, serve a simple stats page
    char accept_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept", accept_hdr, sizeof(accept_hdr)) == ESP_OK && strstr(accept_hdr, "text/html"))
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        SEND_HTML_CHUNK("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Device Stats</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
        SEND_HTML_CHUNK("<link rel='stylesheet' href='/main.css'>");
        SEND_HTML_CHUNK("<script src='/main.js'></script>");
        SEND_HTML_CHUNK("</head><body><div class='container'>");
        
        // Boot status warning banner
        secboot_status_t boot_status;
        char boot_history[256] = "";
        int has_failures = 0;
        if (secboot_status_get(&boot_status) == 0 && boot_status.fifo_size > 0) {
            secboot_status_get_history_string(boot_history, sizeof(boot_history));
            // Check for failures in history
            for (int i = 0; i < boot_status.fifo_size; i++) {
                if (boot_status.fifo[i] == SECBOOT_STATUS_FAIL) {
                    has_failures = 1;
                    break;
                }
            }
        }
        
        if (has_failures) {
            SEND_HTML_CHUNK("<div style='background-color:#fff3cd;border:1px solid #ffc107;border-radius:4px;padding:12px;margin:10px 0;color:#856404;'>");
            SEND_HTML_CHUNK("<strong>⚠️ Boot Status Warning:</strong> Recent boot failures detected.<br>");
            SEND_HTML_CHUNK("History: <code id='bootHistoryBanner'>-</code></div>");
        }
        
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
        SEND_HTML_CHUNK("<tr><td>Boot History:</td><td id='statBootHistory'>-</td></tr>");
        SEND_HTML_CHUNK("</table></div></div></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    // Otherwise, serve JSON
    device_stats_t stats;
    monitor_get_device_stats(&stats);
    
    // Get boot status history
    char boot_history[256] = "UNKNOWN";
    secboot_status_get_history_string(boot_history, sizeof(boot_history));
    
    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"free_heap\":%u,\"min_free_heap\":%u,\"uptime_ms\":%llu,\"cpu_load\":%.2f,\"boot_history\":\"%s\"}\n",
             (unsigned int)stats.free_heap,
             (unsigned int)stats.min_free_heap,
             (unsigned long long)stats.uptime_ms,
             stats.cpu_load,
             boot_history);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* HTTP GET handler for /configure */
static esp_err_t configure_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Configure</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
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
    // Door Sensor Mapping section
    SEND_HTML_CHUNK("<h2>Door Sensor Mapping</h2>");
    SEND_HTML_CHUNK("<form method='POST' action='/configure'>");
    {
        char dibuf[512];
        nvm_door_map_block_t map = {0};
        appl_nvm_get_door_map(&map);
        for (int i = 0; i < NUM_DOOR_SENSORS; ++i)
        {
            /* Simple HTML escape for stored name */
            char esc[DOOR_NAME_LEN * 2];
            size_t eo = 0;
            const char *src = map.names[i];
            if (!src)
                src = "";
            for (size_t j = 0; src[j] != '\0' && eo + 8 < sizeof(esc); ++j)
            {
                char c = src[j];
                if (c == '&')
                {
                    esc[eo++] = '&';
                    esc[eo++] = 'a';
                    esc[eo++] = 'm';
                    esc[eo++] = 'p';
                    esc[eo++] = ';';
                }
                else if (c == '<')
                {
                    esc[eo++] = '&';
                    esc[eo++] = 'l';
                    esc[eo++] = 't';
                    esc[eo++] = ';';
                }
                else if (c == '>')
                {
                    esc[eo++] = '&';
                    esc[eo++] = 'g';
                    esc[eo++] = 't';
                    esc[eo++] = ';';
                }
                else if (c == '"')
                {
                    esc[eo++] = '&';
                    esc[eo++] = 'q';
                    esc[eo++] = 'u';
                    esc[eo++] = 'o';
                    esc[eo++] = ';';
                }
                else
                    esc[eo++] = c;
            }
            esc[eo] = '\0';

            // Show label with current saved value in gray if it exists
            if (esc[0] != '\0')
            {
                int n = snprintf(dibuf, sizeof(dibuf),
                                 "<label for='door_%d'>Door Sensor %d: <span style='color:#999;font-weight:normal;'>(saved: %s)</span></label>"
                                 "<input type='text' id='door_%d' name='door_%d' maxlength='%d' value='%s'><br>",
                                 i, i, esc, i, i, DOOR_NAME_LEN - 1, esc);
                if (n > 0)
                    SEND_HTML_CHUNK(dibuf);
            }
            else
            {
                int n = snprintf(dibuf, sizeof(dibuf),
                                 "<label for='door_%d'>Door Sensor %d: <span style='color:#999;font-weight:normal;'>(not configured)</span></label>"
                                 "<input type='text' id='door_%d' name='door_%d' maxlength='%d' value='' placeholder='Enter door name'><br>",
                                 i, i, i, i, DOOR_NAME_LEN - 1);
                if (n > 0)
                    SEND_HTML_CHUNK(dibuf);
            }
        }
    }
    SEND_HTML_CHUNK("<button type='submit'>Save Door Names</button>");
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
    modemanager_notify_activity_auto();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Nodes</title><meta name='viewport' content='width=device-width,initial-scale=1'>");
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

// HTTP POST handler for /subscribe (apply per-node subscription from UI)
static esp_err_t subscribe_post_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    char buf[96];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    // Expected form body: node_id=<n>&mask=<hex or dec>&interval_ms=<n>
    uint32_t node_id = 0, interval_ms = 0;
    unsigned long mask = 0;
    // very small parser: look for substrings
    char *p;
    p = strstr(buf, "node_id=");
    if (p)
        node_id = (uint32_t)atoi(p + 8);
    p = strstr(buf, "mask=");
    if (p)
    {
        // support hex starting with 0x
        if (p[5] == '0' && (p[6] == 'x' || p[6] == 'X'))
            mask = strtoul(p + 5, NULL, 16);
        else
            mask = strtoul(p + 5, NULL, 10);
    }
    p = strstr(buf, "interval_ms=");
    if (p)
        interval_ms = (uint32_t)atoi(p + 12);

    esp_err_t err = nodeio_update_subscription((uint8_t)node_id, (capability_t)mask, interval_ms);
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false}\n");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}\n");
    return ESP_OK;
}

/* HTTP POST handler for /api/request_logs?node_id=N */
static esp_err_t request_logs_post_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();

    // Parse query string for node_id
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Missing node_id parameter\"}\n");
        return ESP_OK;
    }

    char node_id_str[8];
    if (httpd_query_key_value(query, "node_id", node_id_str, sizeof(node_id_str)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Missing node_id parameter\"}\n");
        return ESP_OK;
    }

    // Bounds check on node_id (CRITICAL FIX #3)
    int node_id_int = atoi(node_id_str);
    if (node_id_int < 0 || node_id_int >= MAX_SESSIONS)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf),
                 "{\"ok\":false,\"error\":\"Invalid node_id %d (valid range: 0-%d)\"}\n",
                 node_id_int, MAX_SESSIONS - 1);
        httpd_resp_sendstr(req, err_buf);
        ESP_LOGW("WebServer", "Invalid node_id in POST request: %d", node_id_int);
        return ESP_OK;
    }

    uint8_t node_id = (uint8_t)node_id_int;

    // Optional: parse max_lines from query or POST body (default 300)
    uint16_t max_lines = 300;
    char max_lines_str[8];
    if (httpd_query_key_value(query, "max_lines", max_lines_str, sizeof(max_lines_str)) == ESP_OK)
    {
        int max_lines_int = atoi(max_lines_str);
        if (max_lines_int > 0 && max_lines_int <= 10000) // Cap at 10K lines
        {
            max_lines = (uint16_t)max_lines_int;
        }
        else
        {
            ESP_LOGW("WebServer", "Invalid max_lines value: %d", max_lines_int);
        }
    }

    // Send log request to node
    esp_err_t err = nodeio_request_logs(node_id, max_lines);
    if (err != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char error_buf[128];
        snprintf(error_buf, sizeof(error_buf),
                 "{\"ok\":false,\"error\":\"%s\"}\n", esp_err_to_name(err));
        httpd_resp_sendstr(req, error_buf);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"status\":\"requested\"}\n");
    return ESP_OK;
}

/* HTTP GET handler for /api/get_node_logs?node_id=N */
static esp_err_t get_node_logs_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();

    // Parse query string for node_id
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing node_id parameter\"}\n");
        return ESP_OK;
    }

    char node_id_str[8];
    if (httpd_query_key_value(query, "node_id", node_id_str, sizeof(node_id_str)) != ESP_OK)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing node_id parameter\"}\n");
        return ESP_OK;
    }

    // Bounds check on node_id (CRITICAL FIX #3)
    int node_id_int = atoi(node_id_str);
    if (node_id_int < 0 || node_id_int >= MAX_SESSIONS)
    {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf),
                 "{\"status\":\"error\",\"message\":\"Invalid node_id %d (valid range: 0-%d)\"}\n",
                 node_id_int, MAX_SESSIONS - 1);
        httpd_resp_sendstr(req, err_buf);
        ESP_LOGW("WebServer", "Invalid node_id in GET request: %d", node_id_int);
        return ESP_OK;
    }

    uint8_t node_id = (uint8_t)node_id_int;

    // Get node params
    node_params_t *p_node = nodeio_get_node_params(node_id);
    if (!p_node)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Node not found\"}\n");
        return ESP_OK;
    }

    // Acquire lock to safely read logs (CRITICAL FIX #2)
    nodeio_lock_logs();

    // Check if logs are available
    if (!p_node->logs_available || !p_node->log_lines || p_node->log_count == 0)
    {
        nodeio_unlock_logs();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"pending\",\"message\":\"No logs available yet\"}\n");
        return ESP_OK;
    }

    // Check timeout (clear logs after 60 seconds)
    time_t now = time(NULL);
    if ((now - p_node->log_timestamp) > 60)
    {
        // Clear expired logs (thread-safe, but we already have lock)
        for (int i = 0; i < p_node->log_count; i++)
        {
            if (p_node->log_lines[i])
                free(p_node->log_lines[i]);
        }
        free(p_node->log_lines);
        p_node->log_lines = NULL;
        p_node->log_count = 0;
        p_node->logs_available = false;

        nodeio_unlock_logs();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"expired\",\"message\":\"Logs have expired\"}\n");
        return ESP_OK;
    }

// Limit response size and use efficient batching (MEDIUM FIX #6, #7)
#define MAX_LOG_RESPONSE_SIZE (64 * 1024) // 64KB max JSON response
#define BATCH_SIZE 2048

    // Build JSON response with logs
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"available\",\"total_lines\":");

    char num_buf[16];
    snprintf(num_buf, sizeof(num_buf), "%d", p_node->log_count);
    httpd_resp_sendstr(req, num_buf);
    httpd_resp_sendstr(req, ",\"logs\":[");

    // Use batching to reduce overhead
    char *batch_buf = malloc(BATCH_SIZE);
    if (!batch_buf)
    {
        nodeio_unlock_logs();
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int batch_offset = 0;
    size_t total_response_size = 0;

    for (int i = 0; i < p_node->log_count; i++)
    {
        if (i > 0)
        {
            int written = snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, ",");
            if (written < 0 || batch_offset + written >= BATCH_SIZE - 100)
            {
                httpd_resp_sendstr(req, batch_buf);
                batch_offset = 0;
                total_response_size += strlen(batch_buf);
                if (total_response_size > MAX_LOG_RESPONSE_SIZE)
                {
                    ESP_LOGW("WebServer", "Log response exceeds max size, truncating");
                    break;
                }
            }
            else
            {
                batch_offset += written;
            }
        }

        batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "{\"line\":\"");

        // Escape and add log line
        const char *line = p_node->log_lines[i];
        if (line)
        {
            for (const char *p = line; *p && batch_offset < BATCH_SIZE - 10; p++)
            {
                if (*p == '"' || *p == '\\')
                {
                    batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "\\%c", *p);
                }
                else if (*p == '\n')
                {
                    batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "\\n");
                }
                else if (*p == '\r')
                {
                    batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "\\r");
                }
                else if (*p == '\t')
                {
                    batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "\\t");
                }
                else if (*p < 32 || *p > 126) // Control chars - skip
                {
                    // Skip non-printable characters
                }
                else
                {
                    batch_buf[batch_offset++] = *p;
                }
            }
        }

        batch_offset += snprintf(batch_buf + batch_offset, BATCH_SIZE - batch_offset, "\"}");

        // Flush batch if getting full
        if (batch_offset > BATCH_SIZE - 200)
        {
            httpd_resp_sendstr(req, batch_buf);
            batch_offset = 0;
            total_response_size += strlen(batch_buf);
            if (total_response_size > MAX_LOG_RESPONSE_SIZE)
            {
                ESP_LOGW("WebServer", "Log response size limit reached");
                break;
            }
        }
    }

    // Send remaining batch
    if (batch_offset > 0)
    {
        httpd_resp_sendstr(req, batch_buf);
    }

    free(batch_buf);
    nodeio_unlock_logs();

    httpd_resp_sendstr(req, "]}\n");
    return ESP_OK;
}

/* HTTP POST handler for /configure */
static esp_err_t configure_post_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    // Read POST body robustly based on Content-Length (cap to 4KB)
    size_t content_len = req->content_len;
    const size_t MAX_POST = 4096;
    if (content_len > MAX_POST)
        content_len = MAX_POST;
    char *buf = malloc(content_len + 1);
    if (!buf)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < content_len)
    {
        int r = httpd_req_recv(req, buf + received, content_len - received);
        if (r <= 0)
        {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

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

    /* Parse door name fields: build local copy, then write once */
    nvm_door_map_block_t local = {0};
    appl_nvm_get_door_map(&local);

    ESP_LOGD("WebServer", "POST body: %s", buf);

    for (int i = 0; i < NUM_DOOR_SENSORS; ++i)
    {
        char key[32];
        snprintf(key, sizeof(key), "door_%d=", i);
        char *q = strstr(buf, key);
        if (q)
        {
            q += strlen(key);

            // Find the end of this value (next '&' or end of string)
            char *end = strchr(q, '&');
            size_t val_len = end ? (size_t)(end - q) : strlen(q);

            // Create a temporary null-terminated copy for url_decode
            char *temp_val = strndup(q, val_len);
            if (!temp_val)
            {
                ESP_LOGW("WebServer", "Memory allocation failed for door_%d", i);
                continue;
            }

            ESP_LOGD("WebServer", "Door %d raw value (len=%zu): '%s'", i, val_len, temp_val);

            char val[DOOR_NAME_LEN * 2];
            int __ud_rc = url_decode(temp_val, val, sizeof(val));
            free(temp_val);

            if (__ud_rc < 0)
            {
                ESP_LOGW("WebServer", "URL decode failed for door_%d", i);
                val[0] = '\0';
            }

            ESP_LOGD("WebServer", "Door %d decoded value: '%s'", i, val);
            // Strip newlines and control chars
            for (char *s = val; *s; ++s)
                if (*s == '\n' || *s == '\r')
                    *s = ' ';
            // Check length before copying
            size_t decoded_len = strlen(val);
            if (decoded_len >= DOOR_NAME_LEN)
            {
                ESP_LOGW("WebServer", "Door name %d truncated: %zu chars -> %d chars",
                         i, decoded_len, DOOR_NAME_LEN - 1);
            }
            // Copy into local map, ensure NUL termination
            memset(local.names[i], 0, DOOR_NAME_LEN);
            strncpy(local.names[i], val, DOOR_NAME_LEN - 1);
            ESP_LOGD("WebServer", "Parsed door_%d='%s'", i, local.names[i]);
        }
    }
    // Commit updated map
    ESP_LOGI("WebServer", "Committing door map to NVM");
    bool save_ok = appl_nvm_set_door_map(&local);
    if (save_ok)
    {
        ESP_LOGI("WebServer", "Door map saved successfully");
    }
    else
    {
        ESP_LOGE("WebServer", "Failed to save door map to NVM");
    }

    free(buf);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><script>window.location='/configure';</script></body></html>");
    return ESP_OK;
}
// HTTP GET handler for /
// Handler to serve main.css
static esp_err_t css_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    size_t css_len = webpage_main_css_end - webpage_main_css_start;
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)webpage_main_css_start, css_len);
    return ESP_OK;
}

// Handler to serve main.js
static esp_err_t js_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    size_t js_len = webpage_main_js_end - webpage_main_js_start;
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)webpage_main_js_start, js_len);
    return ESP_OK;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    const char *ssid = wifi_get_ssid();
    ESP_LOGI("WebServer", "SSID for HTML injection: '%s'", ssid ? ssid : "(null)");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // Send static HTML in flash-resident chunks (saves needing a 4KB RAM buffer)
    SEND_HTML_CHUNK("<!DOCTYPE html><html><head><meta charset='UTF-8'><title>FloraLink.Hub</title>");
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
    // Insert a banner element that JS can toggle without reload
    {
        char sbuf[192];
        const char *disp = g_is_light_sleep ? "block" : "none";
        int n2 = snprintf(sbuf, sizeof(sbuf),
                          "<div id='sleepBanner' style='display:%s;color:#d32f2f;font-weight:bold;text-align:center;margin:8px 0;'>On light sleep</div>",
                          disp);
        if (n2 > 0 && n2 < (int)sizeof(sbuf))
        {
            SEND_HTML_CHUNK(sbuf);
        }
        else
        {
            SEND_HTML_CHUNK("<div id='sleepBanner' style='display:none;color:#d32f2f;font-weight:bold;text-align:center;margin:8px 0;'>On light sleep</div>");
        }
    }
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

// Lightweight endpoint to expose current light sleep state without extending ACTIVE
static esp_err_t sleepstatus_get_handler(httpd_req_t *req)
{
    // Intentionally DO NOT call modemanager_notify_activity_auto() here, to avoid extending ACTIVE window
    char resp[64];
    int is_sleep = g_is_light_sleep ? 1 : 0;
    int n = snprintf(resp, sizeof(resp), "{\"light_sleep\":%d}\n", is_sleep);
    if (n < 0)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP GET handler for /luts - expose LUT metadata for dynamic UI
static esp_err_t luts_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    HEAP_TRACE_START("LUTS_GET");

    cJSON *root = cJSON_CreateObject();
    cJSON *sensors = cJSON_CreateArray();
    cJSON *services = cJSON_CreateArray();

    const field_lookup_t *s_lut = nodeio_get_sensors_lut();
    size_t s_count = nodeio_get_sensors_lut_count();
    for (size_t i = 0; i < s_count; ++i)
    {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", s_lut[i].name);
        cJSON_AddNumberToObject(it, "cap_mask", (double)(unsigned)s_lut[i].cap);
        cJSON_AddNumberToObject(it, "loc", (int)s_lut[i].loc);
        cJSON_AddNumberToObject(it, "elem_count", (int)s_lut[i].elem_count);
        cJSON_AddItemToArray(sensors, it);
    }

    const field_lookup_t *sv_lut = nodeio_get_services_lut();
    size_t sv_count = nodeio_get_services_lut_count();
    for (size_t i = 0; i < sv_count; ++i)
    {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "name", sv_lut[i].name);
        cJSON_AddNumberToObject(it, "cap_mask", (double)(unsigned)sv_lut[i].cap);
        cJSON_AddNumberToObject(it, "loc", (int)sv_lut[i].loc);
        cJSON_AddNumberToObject(it, "elem_count", (int)sv_lut[i].elem_count);
        cJSON_AddItemToArray(services, it);
    }

    cJSON_AddItemToObject(root, "sensors", sensors);
    cJSON_AddItemToObject(root, "services", services);

    char *out = cJSON_PrintUnformatted(root);
    if (out)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        cJSON_free(out);
    }
    else
    {
        httpd_resp_send_500(req);
    }

    cJSON_Delete(root);
    HEAP_TRACE_END_DEFAULT();
    return ESP_OK;
}

// HTTP GET handler for /door_map - return JSON array of configured door names
static esp_err_t door_map_get_handler(httpd_req_t *req)
{
    modemanager_notify_activity_auto();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    nvm_door_map_block_t map = {0};
    appl_nvm_get_door_map(&map);
    for (int i = 0; i < NUM_DOOR_SENSORS; ++i)
    {
        /* map.names[i] is an array (never NULL). Check first char to see if name is present. */
        cJSON_AddItemToArray(arr, cJSON_CreateString(map.names[i][0] ? map.names[i] : ""));
    }
    cJSON_AddItemToObject(root, "doors", arr);
    char *out = cJSON_PrintUnformatted(root);
    if (out)
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, strlen(out));
        cJSON_free(out);
    }
    else
    {
        httpd_resp_send_500(req);
    }
    cJSON_Delete(root);
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
    // increase number of uri handlers.
    // We register a number of HTML, API and static handlers and also the WebSocket
    // handler later; allocate sufficient slots to avoid registration failure.
    config_http.max_uri_handlers = 20;

    // Fix timeout issues - increase timeouts and enable keep-alive
    config_http.recv_wait_timeout = 60; // 60 seconds instead of 5
    config_http.send_wait_timeout = 60; // 60 seconds instead of 5
    // Disable HTTP keep-alive to avoid per-connection memory lingering
    config_http.keep_alive_enable = false;
    // The following are ignored when keep-alive is disabled; left here for quick re-enable if needed
    // config_http.keep_alive_idle = 120;    // Keep-alive idle time: 2 minutes
    // config_http.keep_alive_interval = 30; // Keep-alive interval: 30 seconds
    // config_http.keep_alive_count = 3;     // Keep-alive retry count
    config_http.lru_purge_enable = true; // Enable LRU purge to handle stale connections

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
    httpd_uri_t subscribe_post_uri = {
        .uri = "/subscribe",
        .method = HTTP_POST,
        .handler = subscribe_post_handler,
        .user_ctx = NULL};
    httpd_uri_t request_logs_post_uri = {
        .uri = "/api/request_logs",
        .method = HTTP_POST,
        .handler = request_logs_post_handler,
        .user_ctx = NULL};
    httpd_uri_t get_node_logs_uri = {
        .uri = "/api/get_node_logs",
        .method = HTTP_GET,
        .handler = get_node_logs_handler,
        .user_ctx = NULL};
    httpd_uri_t sleepstatus_uri = {
        .uri = "/sleepstatus",
        .method = HTTP_GET,
        .handler = sleepstatus_get_handler,
        .user_ctx = NULL};
    httpd_uri_t luts_uri = {
        .uri = "/luts",
        .method = HTTP_GET,
        .handler = luts_get_handler,
        .user_ctx = NULL};
    httpd_uri_t door_map_uri = {
        .uri = "/door_map",
        .method = HTTP_GET,
        .handler = door_map_get_handler,
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
    httpd_register_uri_handler(server, &luts_uri);
    httpd_register_uri_handler(server, &door_map_uri);
    httpd_register_uri_handler(server, &sleepstatus_uri);
    httpd_register_uri_handler(server, &subscribe_post_uri);
    httpd_register_uri_handler(server, &request_logs_post_uri);
    httpd_register_uri_handler(server, &get_node_logs_uri);

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

/* Server health monitoring function */
void webserver_health_monitor(void)
{
    static bool first_run = true;
    static size_t previous_free_heap = 0;
    static int low_heap_count = 0;
    static int leak_detection_count = 0;
    static int consecutive_drops = 0; // Track sustained memory drops
    static size_t baseline_heap = 0;  // Baseline for trend detection

    if (first_run)
    {
        ESP_LOGI(TAG, "Server health monitor started");
        first_run = false;
        baseline_heap = esp_get_free_heap_size();
    }

    HEAP_TRACE_START("HEALTH_MONITOR");

    // Check if server handle is still valid
    if (server == NULL)
    {
        ESP_LOGE(TAG, "Server handle is NULL - server may have crashed!");
        // Attempt restart - could implement restart logic here
        HEAP_TRACE_END_DEFAULT();
        return;
    }

    // Get detailed server status information
    size_t open_fds = 0;

    // Get number of active client connections
    // We need to provide a temporary array to get the count
    int temp_fds[20];     // Temporary array (should be >= max_open_sockets)
    size_t fd_count = 20; // Maximum we can handle
    esp_err_t fd_result = httpd_get_client_list(server, &fd_count, temp_fds);
    if (fd_result == ESP_OK)
    {
        open_fds = fd_count; // fd_count now contains actual number of connections
    }

    // Get global user context (can be used to check server state)
    void *global_ctx = httpd_get_global_user_ctx(server);

    // Get transport context
    void *transport_ctx = httpd_get_global_transport_ctx(server);

    // Log comprehensive health status
    ESP_LOGD(TAG, "Server health check: OK");
    ESP_LOGD(TAG, "  - Uptime: %llu ms", esp_timer_get_time() / 1000);
    ESP_LOGD(TAG, "  - Active client connections: %zu", open_fds);
    ESP_LOGD(TAG, "  - Global context: %s", global_ctx ? "set" : "null");
    ESP_LOGD(TAG, "  - Transport context: %s", transport_ctx ? "set" : "null");

    // Get memory info
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    ESP_LOGD(TAG, "  - Free heap: %zu bytes (min: %zu bytes)", free_heap, min_free_heap);

    // Quick health checks
    if (open_fds > 10) // Warn if too many open connections
    {
        ESP_LOGW(TAG, "High number of open connections: %zu", open_fds);
    }

    // Enhanced memory leak detection with trend analysis
    size_t current_free_heap = esp_get_free_heap_size();

    // Memory leak detection - require sustained drops to reduce false positives
    if (previous_free_heap > 0)
    {
        if (current_free_heap < previous_free_heap)
        {
            size_t heap_drop = previous_free_heap - current_free_heap;

            // Only count drops > 1KB as significant (filters out normal fluctuations)
            if (heap_drop > 1024)
            {
                consecutive_drops++;
                ESP_LOGD(TAG, "Heap dropped %zu bytes (consecutive: %d)", heap_drop, consecutive_drops);

                // Only report as leak if we see 3+ consecutive drops
                if (consecutive_drops >= 3)
                {
                    size_t total_loss = baseline_heap - current_free_heap;
                    ESP_LOGW(TAG, "Potential memory leak: %d consecutive drops, total loss %zu bytes (baseline %zu -> %zu)",
                             consecutive_drops, total_loss, baseline_heap, current_free_heap);
                    leak_detection_count++;

                    // Reset baseline to current for tracking future trends
                    baseline_heap = current_free_heap;
                    consecutive_drops = 0;
                }
            }
            else
            {
                // Small drop (<1KB) - likely just normal operation, reset counter
                consecutive_drops = 0;
            }
        }
        else
        {
            // Heap increased or stayed same - reset counter and update baseline
            if (consecutive_drops > 0)
            {
                ESP_LOGD(TAG, "Heap recovered: %zu bytes (was dropping for %d checks)",
                         current_free_heap - previous_free_heap, consecutive_drops);
            }
            consecutive_drops = 0;

            // Update baseline if heap recovered significantly
            if (current_free_heap > baseline_heap)
            {
                baseline_heap = current_free_heap;
            }
        }
    }

    // Progressive heap warnings
    if (current_free_heap < 100000) // First warning at 100KB
    {
        ESP_LOGW(TAG, "Low heap warning: %zu bytes remaining", current_free_heap);
        low_heap_count++;
    }
    if (current_free_heap < 50000) // Critical warning at 50KB
    {
        ESP_LOGE(TAG, "CRITICAL heap warning: %zu bytes remaining", current_free_heap);
    }
    if (current_free_heap < 10000) // Emergency at 10KB
    {
        ESP_LOGE(TAG, "EMERGENCY: Only %zu bytes heap remaining - system may crash!", current_free_heap);
        // Could trigger emergency cleanup or restart here
    }

    // Log leak statistics if any detected
    if (leak_detection_count > 0)
    {
        ESP_LOGE(TAG, "Memory leak summary: %d leaks detected, %d low heap warnings",
                 leak_detection_count, low_heap_count);
    }

    previous_free_heap = current_free_heap;

    HEAP_TRACE_END_DEFAULT();
}
