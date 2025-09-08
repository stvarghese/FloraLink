#include "websockserver.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "websockserver";
static void (*receive_callback)(int client_fd, const char *, size_t) = NULL;
static websockserver_close_cb_t close_callback = NULL;

static wss_session_t wss_activesessions[MAX_SESSIONS];

// Add or update a WebSocket session by session_id (array index)
wss_session_t *websockserver_session_update(int client_fd, int session_id)
{
    if (session_id >= MAX_SESSIONS)
        return NULL;
    wss_activesessions[session_id].client_fd = client_fd;
    wss_activesessions[session_id].connected = true;
    return &wss_activesessions[session_id];
}

// Remove a WebSocket session by client_fd, return pointer to session if found
wss_session_t *websockserver_session_remove(int client_fd)
{
    for (int i = 0; i < MAX_SESSIONS; ++i)
    {
        if (wss_activesessions[i].connected && wss_activesessions[i].client_fd == client_fd)
        {
            wss_activesessions[i].connected = false;
            wss_activesessions[i].client_fd = -1;

            return &wss_activesessions[i];
        }
    }
    return NULL;
}

// Find client_fd by session_id (array index)
int websockserver_session_find_fd(int session_id)
{
    if (session_id >= MAX_SESSIONS)
        return -1;
    if (wss_activesessions[session_id].connected)
        return wss_activesessions[session_id].client_fd;
    return -1;
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
    return -1;
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

        if (close_callback)
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
        ESP_LOGI(TAG, "Pong received from client_fd=%d", httpd_req_to_sockfd(req));
        // Reset pong timer
        websockserver_reset_pong_timer(httpd_req_to_sockfd(req));

        return ESP_OK;
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
            receive_callback(client_fd, (const char *)ws_pkt.payload, ws_pkt.len);
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
        .is_websocket = true};
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
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
        .final = true};
    // Use the server handle
    if (!ws_server_handle)
        return false;
    return httpd_ws_send_frame_async(ws_server_handle, client_fd, &ws_pkt) == ESP_OK;
}

bool websockserver_ping(int client_fd)
{
    // Find session Id and check if valid
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id == -1)
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

    // Create and start pong timer
    esp_timer_create_args_t timer_args = {
        .callback = &websockserver_pong_timeout_callback,
        .arg = (void *)client_fd,
        .name = "ws_pong"};
    esp_timer_create(&timer_args, &ws_pong_timers[session_id]);

    esp_timer_start_once(ws_pong_timers[session_id], WSS_PONG_TIMEOUT * 1000000);
    ws_active_pings[session_id] = true;

    return httpd_ws_send_frame_async(ws_server_handle, client_fd, &ws_pkt) == ESP_OK;
}

void websockserver_reset_pong_timer(int client_fd)
{
    // Find session Id and check if valid
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id == -1)
    {
        ESP_LOGW(TAG, "Reset pong timeout: session not found for client_fd=%d", client_fd);
        return;
    }
    ESP_LOGD(TAG, "Resetting pong timeout upon pong rx for client_fd=%d", client_fd);
    esp_timer_stop(ws_pong_timers[session_id]);
    ws_active_pings[session_id] = false;
}

void websockserver_pong_timeout_callback(void *arg)
{
    int client_fd = (int)arg;
    websockserver_session_remove(client_fd);
    int session_id = websockserver_session_find_sessid(client_fd);
    if (session_id != -1)
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
}

void websockserver_set_receive_callback(void (*callback)(int client_fd, const char *data, size_t len))
{
    receive_callback = callback;
}

void websockserver_set_close_callback(websockserver_close_cb_t cb)
{
    close_callback = cb;
}
