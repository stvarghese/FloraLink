# NodeIO System Documentation

## Overview

The NodeIO system is FloraLink's core IoT communication framework that enables real-time bidirectional communication between the ESP32-C3 hub and multiple sensor nodes via WebSocket connections. It implements a structured JSON-over-WebSocket protocol with capability-based filtering, subscription management, and automatic node lifecycle handling.

## Key Features

### 🌐 **WebSocket-Based Communication**
- Real-time bidirectional communication via `/ws` endpoint
- Text frame-based JSON messaging with structured protocol
- Automatic connection management and session tracking
- Built-in ping/pong mechanism for connection health monitoring

### 🔒 **Structured Protocol**
- Magic number validation (0xBEEFBEEF) for message authenticity
- Type-safe message routing with extensible message types
- Sequence numbering for message ordering and duplicate detection
- Timestamp-based synchronization support

### 🎯 **Capability-Driven Architecture**
- Dynamic sensor/service discovery through capability masks
- Selective data filtering based on advertised capabilities
- Extensible sensor types (temperature, humidity, moisture, distance, light)
- Service integration (diagnostics, OTA updates)

### 📊 **Subscription Management**
- Hub-controlled subscription system with configurable intervals
- Selective data publishing based on subscription filters
- Automatic subscription state tracking per node
- Dynamic subscription parameter updates

### 🔧 **Node Lifecycle Management**
- Automatic node registration and capability negotiation
- Connection state tracking (disconnected, connecting, connected)
- Graceful disconnect handling with cleanup
- Session timeout and error recovery

## Architecture

### Component Structure
```
NodeIO System
├── 🔌 nodeio.c/h           # Core communication logic
├── 📋 nodeioprotocol.c/h   # Protocol definitions & constants  
├── 🌐 websockserver.c/h    # WebSocket transport layer
├── 📡 WebSocket Endpoint   # /ws - JSON text frames
└── 🧪 test/testnode.py     # Python test client (up to 8 nodes)
```

### Data Flow
```
Node Device ↔ WebSocket (/ws) ↔ NodeIO Handler ↔ Hub Services
     ↓              ↓                ↓              ↓
JSON Messages → Text Frames → Message Router → Data Processing
```

## Protocol Specification

### Message Envelope Structure
All messages follow this JSON envelope format:

```json
{
    "magic": 3203391147,           // 0xBEEFBEEF - Protocol validation
    "type": "message_type",        // Message type string (see types below)
    "node_id": 1,                  // Node identifier (0-7)
    "seq_num": 42,                 // Monotonic sequence number
    "timestamp": 1733333333,       // Unix epoch timestamp
    "payload": [...]               // Type-specific payload array (optional)
}
```

### Message Types

#### Connection Management
- **`connect`** - Node registration with capability advertisement
- **`connect_response`** - Hub acknowledgment with connection status
- **`disconnect_request`** - Graceful disconnection request
- **`heartbeat`** / **`heartbeat_ack`** - Application-level keepalive
- **`ping`** / **`pong`** - Low-level connectivity check

#### Data Exchange
- **`node_data`** - Sensor readings and diagnostic information
- **`notify_subscription`** - Hub subscription configuration
- **`poll_data`** - Hub request for immediate data
- **`ack`** - Generic acknowledgment

#### Service Management
- **`diagnostics`** / **`diagnostic_request`** - Health monitoring
- **`ota_request`** / **`ota_status`** - Over-the-air updates (planned)
- **`error`** - Error reporting and handling

### Capability System

Nodes advertise their capabilities during connection using a bitmask system:

```c
typedef enum {
    CAP_TEMP = 1 << 0,        // Temperature sensor
    CAP_MOISTURE = 1 << 1,    // Soil moisture sensor  
    CAP_HUMIDITY = 1 << 2,    // Humidity sensor
    CAP_DISTANCE = 1 << 3,    // Ultrasonic distance sensor
    CAP_LIGHTSENSE = 1 << 4,  // Light/LDR sensor
    CAP_LED = 1 << 5,         // LED actuator
    CAP_BUZZER = 1 << 6,      // Buzzer actuator
    CAP_DIAG = 1 << 7,        // Diagnostic services
    CAP_OTA = 1 << 8          // OTA update capability
} capability_flag_t;
```

## Message Examples

### Node Connection
**Node → Hub:**
```json
{
    "magic": 3203391147,
    "type": "connect",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "controller": 1,
    "sw_version": "1.0.3",
    "sensors": ["temperature", "humidity", "distance"],
    "services": ["diagnostics"]
}
```

**Hub → Node:**
```json
{
    "type": "connect_response",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "status": "accepted"
}
```

### Subscription Configuration
**Hub → Node:**
```json
{
    "magic": 3203391147,
    "type": "notify_subscription",
    "node_id": 1,
    "seq_num": 2,
    "timestamp": 1733333338,
    "payload": {
        "status": "subscribed",
        "filter": {
            "sensors": ["temperature", "humidity", "distance"],
            "services": ["diagnostics"]
        },
        "interval": 5000
    }
}
```

### Sensor Data Transmission
**Node → Hub:**
```json
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

### Graceful Disconnection
**Node → Hub:**
```json
{
    "magic": 3203391147,
    "type": "disconnect_request",
    "node_id": 1,
    "seq_num": 99,
    "timestamp": 1733340000
}
```

## API Reference

### Core Functions

| Function | Purpose | Parameters |
|----------|---------|------------|
| `nodeio_init()` | Initialize NodeIO system | None |
| `nodeio_monitor_nodeslist()` | Monitor active nodes | None |
| `nodeio_publish_nodeslist(json, size)` | Get nodes list as JSON | Buffer and size |
| `nodeio_active_nodes_ping()` | Send ping to all nodes | None |
| `nodeio_process_subscription_updates()` | Process pending subscriptions | None |

### Internal Protocol Functions

| Function | Purpose |
|----------|---------|
| `nodeio_handle_message()` | Main message dispatcher |
| `nodeio_parse_message_payload()` | Parse and validate payloads |
| `nodeio_build_node_capmask()` | Build capability mask from JSON |
| `nodeio_type_str_to_enum()` | Convert type strings to enums |
| `nodeio_send_connect_response()` | Send connection acknowledgment |
| `nodeio_handle_connect()` | Process node connection |
| `nodeio_handle_disconnect()` | Process node disconnection |

### Data Structures

#### Node Context
```c
typedef struct {
    node_params_t *p_node;        // Node parameters
    protocol_msg_t *p_msg;        // Current message
    wss_session_t *p_session;     // WebSocket session
    subscribe_config_t subscription; // Subscription config
    bool subscribed;              // Subscription state
    bool subscription_update;     // Update pending flag
    int64_t node_uptime_start;    // Connection timestamp
    int64_t node_uptime;          // Node uptime seconds
} node_context_t;
```

#### Protocol Message
```c
typedef struct {
    uint32_t magic;               // Protocol magic number
    msg_type_t type;              // Message type enum
    uint8_t node_id;              // Node identifier
    uint32_t seq_num;             // Sequence number
    uint32_t timestamp;           // Unix timestamp
    payload_t payload;            // Message payload
} protocol_msg_t;
```

## Configuration

### System Limits
- **Maximum Nodes**: 8 (MAX_SESSIONS)
- **Maximum Payload Items**: 10 per message
- **Node ID Range**: 0-7
- **Protocol Magic**: 0xBEEFBEEF
- **WebSocket Endpoint**: `/ws`

### Capability Registration
Extend capability support by:
1. Adding new `CAP_*` flag to `capability_flag_t`
2. Updating sensor/service lookup tables
3. Implementing parser logic in `nodeio_parse_message_payload()`

### Message Type Extension
Add new message types by:
1. Adding enum value to `msg_type_t`
2. Declaring `MSG_TYP_*` string constant
3. Updating `msg_type_map` in nodeio.c
4. Implementing handler in `nodeio_handle_message()`

## Testing

### Test Node Client
Use the Python test client to simulate multiple nodes:

```powershell
# Run test client with 3 nodes, 2-second interval
python .\test\testnode.py ws://hub-ip/ws 2 3
```

#### Interactive Commands
- `add <id>` / `add <start>-<end>` - Add nodes
- `remove <id>` - Remove nodes  
- `pause <id>` / `resume <id>` - Control nodes
- `status <id>` - Check node status
- `list` - List all nodes
- `wipe` - Remove all nodes
- `exit` - Quit client

### Connection Testing
```powershell
# Test WebSocket connection
python -c "
import asyncio
import websockets
async def test():
    uri = 'ws://192.168.1.100/ws'
    async with websockets.connect(uri) as ws:
        print('Connected successfully')
asyncio.run(test())
"
```

## Integration Points

### Web Interface
- **`/nodes`** - Node management UI
- **`/nodeslist`** - JSON API for active nodes
- **`/stats`** - System statistics with node counts

### Task Integration
- Called from `init_task` in `main/tasks.c`
- Integrates with WebSocket server lifecycle
- Monitors subscription updates periodically

### Memory Management
- Uses heap tracing patterns (`HEAP_TRACE_START/END`)
- Automatic cleanup on node disconnection
- JSON memory management with `cJSON_Delete()`

## Error Handling

### Connection Errors
- Invalid magic number → Connection rejected
- Unknown message type → Error response sent
- Invalid node_id → Connection rejected
- Capability mismatch → Filtered silently

### Recovery Mechanisms
- Automatic session cleanup on disconnect
- Graceful handling of malformed JSON
- Sequence number validation and logging
- WebSocket ping/pong timeout handling

## Security Considerations

### Protocol Security
- Magic number prevents accidental connections
- Node ID validation prevents session hijacking
- Capability filtering prevents unauthorized data access
- Sequence numbering detects replay attacks

### Network Security
- WebSocket connections over local network only
- No external internet access required
- Captive portal for initial WiFi configuration
- No persistent credential storage in protocol

## Performance Characteristics

### Throughput
- **Message Rate**: ~100 messages/second per node
- **Concurrent Nodes**: Up to 8 simultaneous connections
- **Latency**: <10ms for local network communication
- **Memory**: ~200 bytes per active node context

### Resource Usage
- **RAM**: ~1.6KB for 8 node contexts
- **CPU**: Minimal overhead, event-driven architecture
- **Network**: JSON overhead ~100 bytes per sensor reading
- **Storage**: No persistent storage required

## Troubleshooting

### Common Issues

#### Nodes Not Connecting
1. Check WebSocket endpoint: `ws://hub-ip/ws`
2. Verify magic number: 0xBEEFBEEF (3203391147)
3. Ensure node_id in range 0-7
4. Check WiFi connectivity

#### Data Not Received
1. Verify node capabilities match subscription filter
2. Check subscription status in logs
3. Ensure proper JSON message format
4. Validate timestamp and sequence numbers

#### Connection Drops
1. Check WebSocket ping/pong mechanism
2. Verify network stability
3. Monitor ESP32 memory usage
4. Check for JSON parsing errors

### Debug Commands
```c
// Enable NodeIO debug logging
esp_log_level_set("nodeio", ESP_LOG_DEBUG);

// Monitor active sessions
nodeio_monitor_nodeslist();

// Check subscription states
nodeio_process_subscription_updates();
```

## Future Enhancements

### Planned Features
- **OTA Updates**: Complete over-the-air update implementation
- **Authentication**: Node authentication and authorization
- **Encryption**: WebSocket message encryption support
- **Compression**: JSON payload compression for bandwidth optimization
- **Persistence**: Node configuration persistence across reboots

### Protocol Extensions
- **Binary Payloads**: Support for non-JSON payload types
- **Message Priority**: Priority-based message queuing
- **Batch Messages**: Multiple sensor readings per message
- **Event Streaming**: Real-time event notification system

---

## Quick Reference

### Essential Message Flow
```
1. Node connects with capabilities → Hub accepts/rejects
2. Hub sends subscription config → Node acknowledges  
3. Node sends periodic data → Hub processes and stores
4. Hub can request immediate data → Node responds
5. Node disconnects gracefully → Hub cleans up
```

### Key Files
- `main/nodeio.c/h` - Core implementation
- `main/nodeioprotocol.c/h` - Protocol definitions
- `main/samplenodemsg.json` - Example message format
- `test/testnode.py` - Python test client

### Build Integration
The NodeIO system is automatically built with the main project:
```powershell
idf.py build
idf.py -p COM9 flash monitor
```

Look for `[nodeio]` log entries to monitor system activity.