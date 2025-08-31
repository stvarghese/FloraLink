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
    if (session_id < 0 || session_id >= MAX_SESSIONS)
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
    if (session_id < 0 || session_id >= MAX_SESSIONS)
        return -1;
    if (wss_activesessions[session_id].connected)
        return wss_activesessions[session_id].client_fd;
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
        websockserver_session_remove(client_fd);
        if (close_callback)
        {
            close_callback(client_fd);
        }
        ESP_LOGI(TAG, "WebSocket closed: fd=%d", client_fd);
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
        .is_websocket = true};
    return httpd_register_uri_handler(server_handle, &ws_uri) == ESP_OK;
}

bool websockserver_send(int client_fd, const char *data, size_t len)
{
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)data,
        .len = len,
        .final = true};
    // Find the server handle (assume only one server for now)
    extern httpd_handle_t global_server_handle;
    if (!global_server_handle)
        return false;
    return httpd_ws_send_frame_async(global_server_handle, client_fd, &ws_pkt) == ESP_OK;
}

void websockserver_set_receive_callback(void (*callback)(int client_fd, const char *data, size_t len))
{
    receive_callback = callback;
}

void websockserver_set_close_callback(websockserver_close_cb_t cb)
{
    close_callback = cb;
}
