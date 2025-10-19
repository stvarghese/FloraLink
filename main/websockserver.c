#include "websockserver.h"
#include "esp_log.h"
#include "commonutils.h"
#include <string.h>

// Define pong timeout in seconds
#define WSS_PONG_TIMEOUT 10
// Threshold (ms) above which pong response times are considered unusual and logged
#ifndef WEBSOCK_PONG_WARN_MS
#define WEBSOCK_PONG_WARN_MS 100
#endif

#define MISSING_SESSID -1

// Timer to keep track and expire on missing pongs
static esp_timer_handle_t ws_pong_timers[MAX_SESSIONS] = {NULL};

// Timestamps to keep track of pong response times
static uint64_t ws_ping_timestamps[MAX_SESSIONS] = {0};
static uint64_t ws_pong_timestamps[MAX_SESSIONS] = {0};

// Keep track of active pings
static bool ws_active_pings[MAX_SESSIONS] = {false};

// Pointer to server handle to be used after successful init
static httpd_handle_t ws_server_handle = NULL;

static const char *TAG = "websockserver";
static void (*receive_callback)(int client_fd, const char *, size_t) = NULL;
static websockserver_close_cb_t close_callback = NULL;

static wss_session_t wss_activesessions[MAX_SESSIONS];

// Add or update a WebSocket session by session_id (array index)
wss_session_t *websockserver_session_update(int client_fd, int session_id)
{
    HEAP_TRACE_START("SESSION_UPDATE");

    if (session_id >= MAX_SESSIONS)
    {
        ESP_LOGW(TAG, "Max sessions reached: session_id=%d, client_fd=%d", session_id, client_fd);
        return NULL;
    }
    wss_activesessions[session_id].client_fd = client_fd;
    wss_activesessions[session_id].connected = true;
    ESP_LOGI(TAG, "Session context updated for node: session_id=%d, client_fd=%d", session_id, client_fd);

    HEAP_TRACE_END_DEFAULT();
    return &wss_activesessions[session_id];
}

// Remove a WebSocket session by client_fd, return pointer to session if found
wss_session_t *websockserver_session_remove(int client_fd)
{
    HEAP_TRACE_START("SESSION_REMOVE");

    ESP_LOGI(TAG, "Removing WebSocket session: client_fd=%d", client_fd);
    for (int i = 0; i < MAX_SESSIONS; ++i)
    {
        if (wss_activesessions[i].connected && wss_activesessions[i].client_fd == client_fd)
        {
            // Clean up any active timer to prevent memory leak
            if (ws_pong_timers[i])
            {
                esp_timer_stop(ws_pong_timers[i]);
                esp_timer_delete(ws_pong_timers[i]);
                ws_pong_timers[i] = NULL;
            }

            wss_activesessions[i].connected = false;
            wss_activesessions[i].client_fd = MISSING_SESSID;

            HEAP_TRACE_END_DEFAULT();
            return &wss_activesessions[i];
        }
    }

    HEAP_TRACE_END_DEFAULT();
    return NULL;
}

// Find client_fd by session_id (array index)
int websockserver_session_find_fd(int session_id)
{
    if (session_id >= MAX_SESSIONS)
        return MISSING_SESSID;
    if (wss_activesessions[session_id].connected)
        return wss_activesessions[session_id].client_fd;
    return MISSING_SESSID;
}

// Find session_id by client_fd
int websockserver_session_find_sessid(int client_fd)
{
    for (int i = 0; i < MAX_SESSIONS; ++i)
    {
        if (wss_activesessions[i].connected && wss_activesessions[i].client_fd == client_fd)
        {
            return i;
        }
    }
    return MISSING_SESSID;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        // WebSocket handshake
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = NULL;
    ws_pkt.len = 0;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get frame len: %d", ret);
        return ret;
    }

    // Check for WebSocket close frame
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        int client_fd = httpd_req_to_sockfd(req);

        // check if already disconnected
        if ((MISSING_SESSID != websockserver_session_find_sessid(client_fd)) && close_callback)
        {
            close_callback(client_fd);
        }

        ESP_LOGI(TAG, "WebSocket closed: fd=%d", client_fd);
        return ESP_OK;
    }

    // Check for pong frame
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG)
    {
        // Indicate pong reception
        // ESP_LOGD(TAG, "Pong received from client_fd=%d", httpd_req_to_sockfd(req));
        // Reset pong timer
        websockserver_reset_pong_timer(httpd_req_to_sockfd(req));

        return ESP_OK;
    }

    // Check for ping frame
    if (ws_pkt.type == HTTPD_WS_TYPE_PING)
    {
        ESP_LOGD(TAG, "PING received");
        // Reply with PONG (not automatically handled by httpd since .handle_ws_control_frames = true)
        httpd_ws_frame_t ws_pkt = {
            .type = HTTPD_WS_TYPE_PONG,
            .payload = NULL,
            .len = 0,
            .final = true};
        if (!ws_server_handle)
            return ESP_OK;
        return httpd_ws_send_frame_async(ws_server_handle, httpd_req_to_sockfd(req), &ws_pkt) == ESP_OK ? ESP_OK : ESP_FAIL;
    }

    if (ws_pkt.len)
    {
        ws_pkt.payload = malloc(ws_pkt.len + 1);
        if (ws_pkt.payload == NULL)
            return ESP_ERR_NO_MEM;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK && receive_callback)
        {
            ((char *)ws_pkt.payload)[ws_pkt.len] = '\0';
            int client_fd = httpd_req_to_sockfd(req);
            HEAP_TRACE_START("WS_RX");
            receive_callback(client_fd, (const char *)ws_pkt.payload, ws_pkt.len);
            HEAP_TRACE_END_DEFAULT();
        }
        free(ws_pkt.payload);
    }
    return ESP_OK;
}

bool websockserver_init(httpd_handle_t server_handle)
{
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .handle_ws_control_frames = true,
        .is_websocket = true,
        .supported_subprotocol = "arduino"};
    if (httpd_register_uri_handler(server_handle, &ws_uri) == ESP_OK)
    {
        ws_server_handle = server_handle;

        // Reset active pings
        for (int i = 0; i < MAX_SESSIONS; ++i)
        {
            ws_active_pings[i] = false;
        }
        return true;
    }
    return false;
}

bool websockserver_send(int client_fd, const char *data, size_t len)
{
    HEAP_TRACE_START("WS_SEND");

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
        .final = true};
    // Use the server handle
    if (!ws_server_handle)
        return false;

    bool result = httpd_ws_send_frame_async(ws_server_handle, client_fd, &ws_pkt) == ESP_OK;

    HEAP_TRACE_END_DEFAULT();
    return result;
}

bool websockserver_ping(int client_fd)
{
    HEAP_TRACE_START("WS_PING");

    // Find session Id and check if valid
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id == MISSING_SESSID)
    {
        ESP_LOGW(TAG, "Ping: session not found for client_fd=%d", client_fd);
        return false;
    }
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_PING,
        .payload = NULL,
        .len = 0,
        .final = true};
    // Use the server handle
    if (!ws_server_handle)
        return false;

    // Record ping timestamp
    ws_ping_timestamps[session_id] = esp_timer_get_time();

    // Create and start pong timer
    esp_timer_create_args_t timer_args = {
        .callback = &websockserver_pong_timeout_callback,
        .arg = (void *)client_fd,
        .name = "ws_pong",
        .skip_unhandled_events = true // Allow sleep during idle periods
    };
    esp_timer_create(&timer_args, &ws_pong_timers[session_id]);

    esp_timer_start_once(ws_pong_timers[session_id], WSS_PONG_TIMEOUT * 1000000);
    ws_active_pings[session_id] = true;

    bool result = httpd_ws_send_frame_async(ws_server_handle, client_fd, &ws_pkt) == ESP_OK;

    HEAP_TRACE_END_TIMER(); // Timer allocation expected (~896 bytes), only warn on larger leaks
    return result;
}

void websockserver_reset_pong_timer(int client_fd)
{
    HEAP_TRACE_START("RESET_PONG");

    // Find session Id and check if valid
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id == MISSING_SESSID)
    {
        ESP_LOGW(TAG, "Reset pong timeout: session not found for client_fd=%d", client_fd);
        return;
    }
    ESP_LOGD(TAG, "Resetting pong timeout upon pong rx for client_fd=%d", client_fd);

    // Stop and delete the timer to prevent memory leak
    if (ws_pong_timers[session_id])
    {
        esp_timer_stop(ws_pong_timers[session_id]);
        esp_timer_delete(ws_pong_timers[session_id]);
        ws_pong_timers[session_id] = NULL;
    }
    ws_active_pings[session_id] = false;

    // Record pong timestamp
    ws_pong_timestamps[session_id] = esp_timer_get_time();

    // Calculate pong response time and only log if unusually high
    if (ws_ping_timestamps[session_id] > 0)
    {
        uint64_t latency_us = ws_pong_timestamps[session_id] - ws_ping_timestamps[session_id];
        double latency_ms = (double)latency_us / 1000.0;
        if (latency_ms >= WEBSOCK_PONG_WARN_MS)
        {
            ESP_LOGW(TAG, "High pong response time for client_fd=%d: %.3f ms", client_fd, latency_ms);
        }
        else
        {
            ESP_LOGD(TAG, "Pong response time for client_fd=%d: %.3f ms", client_fd, latency_ms);
        }
    }

    HEAP_TRACE_END_DEFAULT();
}

void websockserver_pong_timeout_callback(void *arg)
{
    HEAP_TRACE_START("PONG_TIMEOUT");

    int client_fd = (int)arg;
    // websockserver_session_remove(client_fd);
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id != MISSING_SESSID)
    {
        ws_active_pings[session_id] = false;
        // Also close the underlying socket/session
        if (ws_server_handle)
        {
            httpd_sess_trigger_close(ws_server_handle, client_fd);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Pong timeout: session not found for client_fd=%d", client_fd);
        return;
    }
    ESP_LOGW(TAG, "Pong timeout for client_fd=%d, closing connection", client_fd);
    if (close_callback)
    {
        close_callback(client_fd);
    }

    HEAP_TRACE_END_DEFAULT();
}

void websockserver_set_receive_callback(void (*callback)(int client_fd, const char *data, size_t len))
{
    receive_callback = callback;
}

void websockserver_set_close_callback(websockserver_close_cb_t cb)
{
    close_callback = cb;
}
