# NodeIO API Reference Card

## Quick Protocol Summary

### Message Envelope (All Messages)
```json
{
    "magic": 3203391147,      // 0xBEEFBEEF - Required validation
    "type": "message_type",   // See message types below
    "node_id": 0,            // Node ID (0-7)  
    "seq_num": 1,            // Monotonic sequence number
    "timestamp": 1733333333   // Unix epoch timestamp
}
```

### Core Message Types

| Direction | Type | Purpose | Payload |
|-----------|------|---------|---------|
| Node → Hub | `connect` | Register with capabilities | sensors[], services[] |
| Hub → Node | `connect_response` | Accept/reject connection | status |
| Hub → Node | `notify_subscription` | Configure data publishing | filter, interval |
| Node → Hub | `node_data` | Send sensor readings | payload[] array |
| Node → Hub | `disconnect_request` | Graceful disconnection | None |
| Both | `heartbeat` / `ping` | Keep connection alive | None |

## Connection Flow

### 1. Node Registration
```json
// Node → Hub
{
    "magic": 3203391147,
    "type": "connect",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "controller": 2,
    "sw_version": "1.0.0",
    "sensors": ["temperature", "humidity", "distance"],
    "services": ["diagnostics"]
}
```

### 2. Hub Response
```json
// Hub → Node  
{
    "type": "connect_response",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "status": "accepted"
}
```

### 3. Subscription Setup
```json
// Hub → Node
{
    "type": "notify_subscription",
    "node_id": 1,
    "payload": {
        "status": "subscribed",
        "filter": {
            "sensors": ["temperature", "humidity"],
            "services": ["diagnostics"]
        },
        "interval": 5000
    }
}
```

### 4. Data Publishing
```json
// Node → Hub (every interval)
{
    "magic": 3203391147,
    "type": "node_data",
    "node_id": 1,
    "seq_num": 10,
    "timestamp": 1733338444,
    "payload": [
        {
            "type": "sensor",
            "sensor": {
                "temperature": 22.7,
                "humidity": 45.1,
                "distance": 123.4
            }
        },
        {
            "type": "diagnostics", 
            "diagnostics": {
                "uptime_sec": 864,
                "free_heap": 140000,
                "rssi": -56,
                "error_code": 0
            }
        }
    ]
}
```

## Capability System

### Sensor Capabilities
```c
CAP_TEMP        // Temperature sensor (°C)
CAP_MOISTURE    // Soil moisture (0-1023)
CAP_HUMIDITY    // Humidity sensor (0-100%)
CAP_DISTANCE    // Ultrasonic distance (cm)
CAP_LIGHTSENSE  // Light sensor (0-1023)
```

### Service Capabilities  
```c
CAP_LED         // LED control
CAP_BUZZER      // Buzzer control
CAP_DIAG        // Diagnostic information
CAP_OTA         // OTA update support (planned)
```

### Capability Declaration
```json
// In connect message
"sensors": ["temperature", "humidity", "distance"],
"services": ["diagnostics", "ota"]
```

## Payload Types

### Sensor Payload
```json
{
    "type": "sensor",
    "sensor": {
        "temperature": 22.5,    // °C
        "humidity": 65.0,       // %
        "moisture": 512,        // 0-1023
        "distance": 45.2,       // cm
        "light": 800            // 0-1023
    }
}
```

### Diagnostics Payload
```json
{
    "type": "diagnostics",
    "diagnostics": {
        "uptime_sec": 3600,     // Seconds since boot
        "free_heap": 25600,     // Free RAM in bytes
        "rssi": -45,            // WiFi signal strength (dBm)
        "error_code": 0         // Application error code
    }
}
```

### OTA Status Payload (Planned)
```json
{
    "type": "ota_status",
    "ota_status": {
        "status_code": 200,     // HTTP-style status
        "message": "Updated successfully"
    }
}
```

## C API Functions

### Hub Functions
```c
// Initialize NodeIO system
esp_err_t nodeio_init(void);

// Monitor active nodes 
void nodeio_monitor_nodeslist(void);

// Get nodes list as JSON
size_t nodeio_publish_nodeslist(char *json, size_t json_size);

// Send ping to all active nodes
void nodeio_active_nodes_ping(void);

// Process subscription updates
void nodeio_process_subscription_updates(void);
```

### Protocol Constants
```c
#define PROTOCOL_MAGIC 0xBEEFBEEF
#define PROTOCOL_VERSION 1
#define PROTOCOL_MAX_PAYLOAD_COUNT 10
#define MAX_NODES 8                    // Maximum concurrent nodes
```

### Data Structures
```c
// Node parameters
typedef struct {
    uint8_t node_id;
    controller_type_t controller;
    capability_t capability_mask;
    nodeio_state_t current_state;
    char sw_version[16];
} node_params_t;

// Message envelope
typedef struct {
    uint32_t magic;
    msg_type_t type;
    uint8_t node_id;
    uint32_t seq_num;
    uint32_t timestamp;
    payload_t payload;
} protocol_msg_t;
```

## Test Client Commands

### Basic Usage
```powershell
python testnode.py ws://hub-ip/ws interval nodes
python testnode.py ws://192.168.1.100/ws 2 3   # 2s interval, 3 nodes
```

### Interactive Commands
```
cmd                    # Enter command mode
add 1                  # Add node 1
add 2-5                # Add nodes 2,3,4,5
remove 1               # Remove node 1
pause 2                # Pause node 2 data
resume 2               # Resume node 2 data
status 1               # Show node 1 status
list                   # List all nodes
wipe                   # Remove all nodes
done                   # Exit command mode
exit                   # Quit program
```

## Error Handling

### Common Error Conditions
| Error | Cause | Solution |
|-------|-------|----------|
| Invalid magic | Wrong magic number | Use 0xBEEFBEEF (3203391147) |
| Unknown type | Invalid message type | Check MSG_TYP_* constants |
| Invalid node_id | Out of range 0-7 | Use valid node ID |
| Capability mismatch | Sensor not advertised | Add to capabilities in connect |
| JSON parse error | Malformed JSON | Validate JSON format |

### Debug Log Categories
```c
esp_log_level_set("nodeio", ESP_LOG_DEBUG);        // Protocol messages
esp_log_level_set("websockserver", ESP_LOG_DEBUG); // WebSocket events
esp_log_level_set("webserver", ESP_LOG_DEBUG);     // HTTP requests
```

## Configuration Limits

| Parameter | Limit | Description |
|-----------|-------|-------------|
| Max Nodes | 8 | Concurrent WebSocket connections |
| Max Payload Items | 10 | Items per payload array |
| Node ID Range | 0-7 | Valid node identifiers |
| Message Size | ~4KB | Practical JSON message limit |
| Sequence Numbers | uint32 | Wraps at 4.3 billion |

## WebSocket Details

### Endpoint
```
ws://hub-ip/ws
```

### Subprotocol
```
Sec-WebSocket-Protocol: arduino
```

### Frame Type
```
Text frames only (JSON)
```

### Ping/Pong
```
Hub sends WebSocket PING
Node must respond with PONG
Timeout = configurable (default ~30s)
```

## Integration Points

### Web Interface
- `/nodes` - Node management UI
- `/nodeslist` - JSON API endpoint
- `/stats` - System statistics

### System Integration
- WebSocket server lifecycle
- Task management in `tasks.c`
- Memory tracing integration
- Error reporting system

## Common Usage Patterns

### Simple Sensor Node
```json
// Connect with basic sensors
"sensors": ["temperature", "humidity"]
"services": ["diagnostics"]

// Send data every 5 seconds
"interval": 5000
```

### Complex Weather Station
```json
// Connect with full capabilities
"sensors": ["temperature", "humidity", "distance", "light"]
"services": ["diagnostics", "ota"]

// Higher frequency updates
"interval": 2000
```

### Minimal Diagnostic Node
```json
// Diagnostics only
"sensors": []
"services": ["diagnostics"]

// Infrequent updates
"interval": 30000
```

---

## Quick Checklist

### Node Implementation
- ✅ Include magic number: 3203391147
- ✅ Use valid message types from protocol
- ✅ Keep node_id in range 0-7
- ✅ Increment seq_num for each message
- ✅ Handle subscription configuration
- ✅ Send data at requested interval
- ✅ Implement graceful disconnect

### Hub Integration
- ✅ Initialize with `nodeio_init()`
- ✅ Handle WebSocket connections
- ✅ Process capability negotiation
- ✅ Manage subscription state
- ✅ Clean up on disconnect

**Documentation**: [NODEIO_README.md](NODEIO_README.md) | **Quick Start**: [NODEIO_QUICKSTART.md](NODEIO_QUICKSTART.md)  
**Test**: `python test/testnode.py ws://hub-ip/ws 5 2` | **Build**: `idf.py build && idf.py flash monitor`