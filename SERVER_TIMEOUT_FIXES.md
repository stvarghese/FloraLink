# Server Timeout Issue - Analysis and Fixes

## Problem Description
After approximately 20 minutes of operation, both the HTTP webserver and WebSocket server would become unresponsive, causing:
- Browser pages to become unavailable
- Node disconnections due to unreachable server
- Complete server failure requiring restart

## Root Cause Analysis
The issue was identified as **ESP-IDF HTTP server default timeout configuration**:

1. **Short receive timeout**: `recv_wait_timeout = 5` seconds (too aggressive)
2. **Short send timeout**: `send_wait_timeout = 5` seconds (too aggressive)  
3. **Disabled keep-alive**: `keep_alive_enable = false` (connections not maintained)
4. **WebSocket pong timeout**: Only 5 seconds for pong responses
5. **WiFi power save**: May have been causing WiFi disconnections
6. **No connection monitoring**: No health checks to detect server failures

## Implemented Fixes

### 1. HTTP Server Timeout Configuration (webserver.c)
```c
// Previous: Used default timeouts (5 seconds each)
httpd_config_t config_http = HTTPD_DEFAULT_CONFIG();

// Fixed: Extended timeouts and enabled keep-alive
config_http.recv_wait_timeout = 60;     // 60 seconds instead of 5
config_http.send_wait_timeout = 60;     // 60 seconds instead of 5
config_http.keep_alive_enable = true;   // Enable TCP keep-alive
config_http.keep_alive_idle = 120;      // Keep-alive idle time: 2 minutes
config_http.keep_alive_interval = 30;   // Keep-alive interval: 30 seconds
config_http.keep_alive_count = 3;       // Keep-alive retry count
config_http.lru_purge_enable = true;    // Enable LRU purge for stale connections
```

### 2. HTTP Server Health Monitoring (webserver.c)
Added a dedicated monitoring task that:
- Checks server handle validity every 30 seconds
- Logs health status every 10 minutes
- Can detect and potentially restart failed servers

### 3. WebSocket Timeout Improvements (websockserver.c)
```c
// Previous: 5 second pong timeout
#define WSS_PONG_TIMEOUT 5

// Fixed: 30 second pong timeout
#define WSS_PONG_TIMEOUT 30
```

Added periodic ping functionality:
- `websockserver_ping_all_clients()` function
- Called every 60 seconds to keep connections alive
- Prevents idle connection timeouts

### 4. WiFi Stability Improvements (wifi_setup.c)
```c
// Disable WiFi power save to prevent connection drops
ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
```

### 5. Connection Keep-Alive Monitoring (tasks.c)
Added to the 1-second monitor task:
```c
// Ping WebSocket clients every 60 seconds to keep connections alive
static int ws_ping_counter = 0;
if (++ws_ping_counter >= 60)
{
    websockserver_ping_all_clients();
    ws_ping_counter = 0;
}
```

## Expected Results
With these fixes, the system should:

1. **Maintain stable HTTP connections** for much longer periods (60+ minutes instead of 20)
2. **Keep WebSocket connections alive** through periodic pings
3. **Prevent WiFi-related disconnections** by disabling power save
4. **Provide server health monitoring** to detect and log any future issues
5. **Handle stale connections gracefully** through LRU purging

## Testing Recommendations

1. **Long-term stability test**: Run for 2+ hours and verify:
   - Web interface remains accessible
   - WebSocket connections stay active
   - Node connections remain stable

2. **Connection stress test**: Open multiple browser tabs/connections

3. **Network disruption test**: Temporarily disconnect/reconnect WiFi

4. **Monitor logs** for:
   - Server health check messages (every 10 minutes)
   - WebSocket ping activities (every 60 seconds)
   - Any timeout or connection error messages

## Files Modified
- `main/webserver.c` - HTTP server timeout configuration and health monitoring
- `main/websockserver.c` - WebSocket timeout and ping functionality  
- `main/websockserver.h` - Added ping function declaration
- `main/wifi_setup.c` - Disabled WiFi power save
- `main/tasks.c` - Added WebSocket keep-alive to monitor task

## Configuration Values Summary
| Setting | Previous | New | Reason |
|---------|----------|-----|---------|
| HTTP recv_wait_timeout | 5s | 60s | Prevent premature disconnections |
| HTTP send_wait_timeout | 5s | 60s | Prevent send failures |
| HTTP keep_alive_enable | false | true | Maintain connections |
| HTTP keep_alive_idle | 0 | 120s | 2-minute idle before keep-alive |
| HTTP keep_alive_interval | 0 | 30s | 30-second keep-alive checks |
| WebSocket pong_timeout | 5s | 30s | More tolerant of network delays |
| WiFi power save | Default | WIFI_PS_NONE | Prevent WiFi disconnections |

This comprehensive fix addresses the server timeout issue from multiple angles to ensure robust, long-term operation.