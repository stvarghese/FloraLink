#ifndef WEBSOCKSERVER_H
#define WEBSOCKSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"
#include "esp_timer.h"

#define MAX_SESSIONS 8

typedef struct
{
    int client_fd;
    bool connected;
} wss_session_t;

// Define pong timeout in seconds
#define WSS_PONG_TIMEOUT 5

// Timer to keep track of time since ping for each client
static esp_timer_handle_t ws_pong_timers[MAX_SESSIONS] = {NULL};

// Keep track of active pings
static bool ws_active_pings[MAX_SESSIONS] = {false};

// Pointer to server handle to be used after successful init
static httpd_handle_t ws_server_handle = NULL;

// Initialize WebSocket server (registers /ws endpoint)
bool websockserver_init(httpd_handle_t server_handle);

// Send data to a connected client (by session socket fd)
bool websockserver_send(int client_fd, const char *data, size_t len);

// Ping a connected client
bool websockserver_ping(int client_fd);

// Pong a connected client
// bool websockserver_pong(int client_fd);
// Pong handled automatically by httpd

// Pong timeout handler
void websockserver_pong_timeout(int client_fd);

// Reset pong timeout for a connected client
void websockserver_reset_pong_timer(int client_fd);

// Pong timeout callback for esp_timer
void websockserver_pong_timeout_callback(void *arg);

// Set callback for received data from any client
void websockserver_set_receive_callback(void (*callback)(int client_fd, const char *data, size_t len));

// Optional: Close a client connection
typedef void (*websockserver_close_cb_t)(int client_fd);
void websockserver_set_close_callback(websockserver_close_cb_t cb);

// Session management functions
wss_session_t *websockserver_session_update(int client_fd, int session_id);
wss_session_t *websockserver_session_remove(int client_fd);
int websockserver_session_find_fd(int session_id);
int websockserver_session_find_sessid(int client_fd);

#endif // WEBSOCKSERVER_H
