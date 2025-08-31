#ifndef WEBSOCK_H
#define WEBSOCK_H

#include <stdbool.h>
#include <stddef.h>

// Initialize WebSocket client
bool websock_init(const char *uri);

// Send data over WebSocket
bool websock_send(const char *data, size_t len);

// Set callback for received data
void websock_set_receive_callback(void (*callback)(const char *data, size_t len));

// Close WebSocket connection
void websock_close(void);

#endif // WEBSOCK_H
