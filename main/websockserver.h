#ifndef WEBSOCKSERVER_H
#define WEBSOCKSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"

#define MAX_SESSIONS 8

typedef struct
{
    int client_fd;
    bool connected;
} wss_session_t;

// Pointer to server handle to be used after successful init
static httpd_handle_t ws_server_handle = NULL;

// Initialize WebSocket server (registers /ws endpoint)
bool websockserver_init(httpd_handle_t server_handle);

// Send data to a connected client (by session socket fd)
bool websockserver_send(int client_fd, const char *data, size_t len);

// Set callback for received data from any client
void websockserver_set_receive_callback(void (*callback)(int client_fd, const char *data, size_t len));

// Optional: Close a client connection
typedef void (*websockserver_close_cb_t)(int client_fd);
void websockserver_set_close_callback(websockserver_close_cb_t cb);

// Session management functions
wss_session_t *websockserver_session_update(int client_fd, uint8_t session_id);
wss_session_t *websockserver_session_remove(int client_fd);
int websockserver_session_find_fd(uint8_t session_id);
uint8_t websockserver_session_find_sessid(int client_fd);

#endif // WEBSOCKSERVER_H
