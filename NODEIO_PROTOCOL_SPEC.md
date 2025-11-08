# NodeIO Protocol Specification

## Version 1.0

### Abstract
This document defines the NodeIO protocol, a JSON-over-WebSocket communication protocol for IoT sensor networks. The protocol enables real-time bidirectional communication between a central hub (ESP32-C3) and multiple sensor nodes with capability-based data filtering and subscription management.

## 1. Protocol Overview

### 1.1 Transport Layer
- **Transport**: WebSocket (RFC 6455)
- **Message Format**: JSON text frames
- **Encoding**: UTF-8
- **Endpoint**: `/ws`
- **Subprotocol**: `arduino` (optional)

### 1.2 Protocol Features
- Magic number validation for message authenticity
- Type-safe message routing with extensible message types
- Capability-based sensor and service discovery
- Subscription-based data publishing with configurable intervals
- Sequence numbering for message ordering and duplicate detection
- Timestamp synchronization support

### 1.3 Connection Model
- **Hub**: Central coordinator (ESP32-C3 device)
- **Nodes**: Sensor devices (Arduino, ESP8266, ESP32, etc.)
- **Sessions**: WebSocket connections (max 8 concurrent)
- **Node IDs**: Unique identifiers (0-7) per session

## 2. Message Structure

### 2.1 Base Message Envelope
All **node → hub** messages MUST contain these fields:

```json
{
    "magic": 3203391147,        // Protocol validation (0xBEEFBEEF)
    "type": "message_type",     // Message type identifier
    "node_id": 0,              // Node identifier (0-7)
    "seq_num": 1,              // Sequence number (monotonic)
    "timestamp": 1733333333     // Unix epoch timestamp
}
```

**Hub → node** messages omit the `magic` field in the current implementation:

```json
{
    "type": "message_type",     // Message type identifier
    "node_id": 0,              // Node identifier (0-7)
    "seq_num": 1,              // Sequence number (monotonic)
    "timestamp": 1733333333     // Unix epoch timestamp
}
```

### 2.2 Field Specifications

#### 2.2.1 Magic Number
- **Type**: uint32
- **Value**: 3203391147 (0xBEEFBEEF)
- **Purpose**: Protocol validation and version identification
- **Validation**: Messages with incorrect magic MUST be rejected
- **Direction**: Required for node → hub, omitted for hub → node

#### 2.2.2 Message Type
- **Type**: string
- **Values**: See Section 3 (Message Types)
- **Purpose**: Determines message handling and payload structure
- **Validation**: Unknown types SHOULD trigger error response

#### 2.2.3 Node ID
- **Type**: uint8 (JSON number, not string)
- **Range**: 0-7 (inclusive)
- **Purpose**: Unique identifier for node within session
- **Validation**: Out-of-range values MUST be rejected

#### 2.2.4 Sequence Number
- **Type**: uint32 (JSON number)
- **Range**: 1 to 4,294,967,295
- **Purpose**: Message ordering and duplicate detection
- **Behavior**: MUST increment for each message sent by node

#### 2.2.5 Timestamp
- **Type**: uint32 (JSON number)
- **Format**: Unix epoch seconds
- **Purpose**: Message timing and synchronization
- **Source**: Node's local time (best effort)

### 2.3 Optional Fields
Additional fields MAY be present based on message type:

```json
{
    // Base envelope fields...
    "controller": 2,            // Controller type (see Section 4.1)
    "sw_version": "1.0.0",     // Software version string
    "sensors": ["temp", "hum"], // Sensor capability list
    "services": ["diag"],       // Service capability list
    "payload": [...]            // Message-specific payload
}
```

## 3. Message Types

### 3.1 Connection Management

#### 3.1.1 Connect Request (`connect`)
**Direction**: Node → Hub  
**Purpose**: Register node and advertise capabilities

```json
{
    "magic": 3203391147,
    "type": "connect",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "controller": 2,                    // Controller type enum
    "sw_version": "1.0.3",             // Software version
    "sensors": ["temperature", "humidity", "distance"],
    "services": ["diagnostics"]
}
```

**Fields**:
- `controller`: Controller type (see Section 4.1)
- `sw_version`: Node software version (max 15 chars)
- `sensors`: Array of sensor capability names
- `services`: Array of service capability names

#### 3.1.2 Connect Response (`connect_response`)
**Direction**: Hub → Node  
**Purpose**: Acknowledge connection attempt

```json
{
    "type": "connect_response",
    "node_id": 1,
    "seq_num": 1,
    "timestamp": 1733333333,
    "status": "accepted"               // "accepted" or "rejected"
}
```

**Fields**:
- `status`: Connection result ("accepted" or "rejected")

#### 3.1.3 Disconnect Request (`disconnect_request`)
**Direction**: Node → Hub  
**Purpose**: Graceful disconnection

```json
{
    "magic": 3203391147,
    "type": "disconnect_request",
    "node_id": 1,
    "seq_num": 99,
    "timestamp": 1733340000
}
```

### 3.2 Subscription Management

#### 3.2.1 Subscription Notification (`notify_subscription`)
**Direction**: Hub → Node  
**Purpose**: Configure data publishing parameters

```json
{
    "magic": 3203391147,
    "type": "notify_subscription",
    "node_id": 1,
    "seq_num": 2,
    "timestamp": 1733333338,
    "payload": {
        "status": "subscribed",         // "subscribed" or "unsubscribed"
        "filter": {
            "sensors": ["temperature", "humidity"],
            "services": ["diagnostics"]
        },
        "interval": 5000               // Publishing interval in ms
    }
}
```

**Payload Fields**:
- `status`: Subscription state
- `filter`: Requested sensor/service filters
- `interval`: Data publishing interval (milliseconds)

### 3.3 Data Exchange

#### 3.3.1 Node Data (`node_data`)
**Direction**: Node → Hub  
**Purpose**: Transmit sensor readings and diagnostics

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

**Payload Structure**:
- `payload`: Array of data items (max 10 items)
- Each item has `type` and corresponding data object

#### 3.3.2 Poll Data Request (`poll_data`)
**Direction**: Hub → Node  
**Purpose**: Request immediate data transmission

```json
{
    "magic": 3203391147,
    "type": "poll_data",
    "node_id": 1,
    "seq_num": 5,
    "timestamp": 1733338000
}

### 3.3.3 Node Event (`node_event`) - Sporadic / Event-driven data
**Direction**: Node → Hub  
**Purpose**: Transmit immediate, event-driven sensor/service data (for example: door open/close, button press).

Format notes:
- `node_event` messages use the same base envelope (see Section 2.1).
- Unlike `node_data` (an array), `node_event` carries a single JSON object in `payload` describing the event.
- A required string field `event_type` indicates the event kind (e.g. `"EVENT_DOOR"`).
- Event-specific fields are encoded as named properties within the `payload` object.
- Supports two formats: per-index keys (`doorsense_N`) or array (`doorsense: [...]`)

**Format 1: Per-index keys** (recommended for sparse events):
```json
{
    "magic": 3203391147,
    "type": "node_event",
    "node_id": 2,
    "seq_num": 42,
    "timestamp": 12345678,
    "payload": {
        "event_type": "EVENT_DOOR",
        "doorsense_1": "OPEN",
        "doorsense_2": "CLOSED"
    }
}
```

**Format 2: Array** (all indices):
```json
{
    "magic": 3203391147,
    "type": "node_event",
    "node_id": 0,
    "seq_num": 43,
    "timestamp": 12345679,
    "payload": {
        "event_type": "EVENT_DOOR",
        "doorsense": ["OPEN", "CLOSED", "UNKNOWN"]
    }
}
```

**Legacy alias**: The hub also accepts `door_state` and `door_state_N` as aliases for `doorsense`.

**Rules and hub behavior (current implementation)**:
- The hub expects `event_type` to be a string and dispatches handling based on that value.
- For `EVENT_DOOR` the hub looks up a sensor LUT entry named `"doorsense"` (this LUT describes capability flag, element count and whether the field is sporadic).
- The hub will only accept and persist an event if the node advertises the corresponding capability (the node's capability mask contains the LUT flag).
- The hub will also verify the LUT declares the field as sporadic (FIELD_LOC_SPORADIC). If the LUT does not mark the field sporadic, the event is rejected.
- Door ownership: Each door index (0-2) can only be owned by one node. The first node to send an event for a door index claims ownership. Subsequent events from other nodes for that door are rejected with a warning.
- Door-state elements are clamped to the LUT-declared `elem_count` (NUM_DOOR_SENSORS = 3). Missing elements are treated as "unknown"; string values like `"OPEN"`/`"CLOSED"` or numeric values (1/0) are accepted and normalized by the hub.
- Sporadic/event writes are stored in the message's `sporadic_data` area (separate from the periodic payload array) so incoming events do not overwrite periodic `node_data` slots.

**Door ownership and display**:
- Each door index can only be claimed by one node
- Nodes should only send events for door indices they own
- The hub publishes door states per-node (each node's `/nodeslist` entry contains only its owned doors)
- Frontend displays doors only on the owning node's card

**Notes on acceptance policy**:
- The hub accepts `node_event` messages based on the node's advertised capability mask: if the node claims the capability (the capability flag in its connect message), the hub will parse and persist the event (subject to LUT validation). The hub does not require a separate server-side subscription mask to accept event payloads. This keeps the hub implementation simple and relies on the node to only send events for which the hub previously requested delivery via the subscription handshake.

**Extensibility**:
- Add new event types by defining an `event_type` string and a corresponding LUT entry for the event's named fields (name, capability flag, elem_count, and `FIELD_LOC_SPORADIC` if appropriate). Update both node and hub LUTs so parsing/serialization remain in sync.

### 3.4 Service Messages

#### 3.4.1 Heartbeat (`heartbeat`)
**Direction**: Both  
**Purpose**: Application-level keepalive

```json
{
    "magic": 3203391147,
    "type": "heartbeat",
    "node_id": 1,
    "seq_num": 15,
    "timestamp": 1733339000
}
```

#### 3.4.2 Error Report (`error`)
**Direction**: Both  
**Purpose**: Error notification

```json
{
    "magic": 3203391147,
    "type": "error",
    "node_id": 1,
    "seq_num": 20,
    "timestamp": 1733339100,
    "payload": {
        "error_code": 500,
        "message": "Sensor reading failed"
    }
}
```

## 4. Data Types and Enumerations

### 4.1 Controller Types
```c
typedef enum {
    CONTROLLER_NODEMCU = 0,     // NodeMCU (ESP8266)
    CONTROLLER_ESP32 = 1,       // ESP32 family
    CONTROLLER_ARDUINO = 2,     // Arduino compatible
    CONTROLLER_UNKNOWN = 3      // Unknown/other
} controller_type_t;
```

### 4.2 Capability Flags
```c
typedef enum {
    CAP_TEMP = 1 << 0,          // Temperature sensor
    CAP_MOISTURE = 1 << 1,      // Soil moisture sensor
    CAP_HUMIDITY = 1 << 2,      // Humidity sensor
    CAP_DISTANCE = 1 << 3,      // Ultrasonic distance
    CAP_LIGHTSENSE = 1 << 4,    // Light sensor
    CAP_LED = 1 << 5,           // LED actuator
    CAP_BUZZER = 1 << 6,        // Buzzer actuator
    CAP_DIAG = 1 << 7,          // Diagnostic services
    CAP_OTA = 1 << 8,           // OTA update capability
    CAP_DOORSENSE = 1 << 9      // Door sensor (sporadic events)
} capability_flag_t;
```

### 4.3 Sensor Data Types

#### 4.3.1 Temperature
- **Unit**: Degrees Celsius (°C)
- **Type**: float
- **Range**: -40.0 to 85.0 (typical sensor range)
- **Precision**: 0.1°C

#### 4.3.2 Humidity
- **Unit**: Percentage (%)
- **Type**: float
- **Range**: 0.0 to 100.0
- **Precision**: 0.1%

#### 4.3.3 Moisture
- **Unit**: ADC counts or percentage
- **Type**: float
- **Range**: 0 to 1023 (10-bit ADC) or 0.0 to 100.0 (%)
- **Precision**: 1 count or 0.1%

#### 4.3.4 Distance
- **Unit**: Centimeters (cm)
- **Type**: float
- **Range**: 2.0 to 400.0 (typical ultrasonic range)
- **Precision**: 0.1 cm

#### 4.3.5 Light
- **Unit**: ADC counts or lux
- **Type**: float
- **Range**: 0 to 1023 (10-bit ADC) or 0 to 65535 (lux)
- **Precision**: 1 count or 1 lux

#### 4.3.6 Door Sensor (Sporadic)
- **Unit**: State (OPEN/CLOSED/UNKNOWN)
- **Type**: string or numeric (1=OPEN, 0=CLOSED, 255=UNKNOWN)
- **Indices**: 0 to 2 (NUM_DOOR_SENSORS = 3)
- **Delivery**: Event-driven via `node_event` message type
- **Ownership**: Each door index can only be claimed by one node (first-come, first-served)
- **Format**: Per-index keys (`doorsense_0`, `doorsense_1`, `doorsense_2`) or array (`doorsense: ["OPEN", "CLOSED", "UNKNOWN"]`)
- **Legacy alias**: `door_state` accepted as equivalent to `doorsense`

### 4.4 Diagnostic Data Types

#### 4.4.1 Uptime
- **Unit**: Seconds
- **Type**: uint32
- **Range**: 0 to 4,294,967,295 (~136 years)
- **Source**: Time since node boot/reset

#### 4.4.2 Free Heap
- **Unit**: Bytes
- **Type**: uint32
- **Range**: 0 to available RAM
- **Purpose**: Memory usage monitoring

#### 4.4.3 RSSI
- **Unit**: dBm
- **Type**: int32
- **Range**: -100 to 0 (typical WiFi range)
- **Purpose**: Signal strength indication

#### 4.4.4 Error Code
- **Unit**: Numeric code
- **Type**: uint32
- **Range**: 0 to 4,294,967,295
- **Purpose**: Application-specific error reporting

## 5. Protocol Behavior

### 5.1 Connection Lifecycle

#### 5.1.1 Establishment
1. Node opens WebSocket connection to hub `/ws`
2. Node sends `connect` message with capabilities
3. Hub validates and responds with `connect_response`
4. Hub sends `notify_subscription` with configuration
5. Node begins data transmission per subscription

#### 5.1.2 Data Exchange
1. Node sends `node_data` at configured intervals
2. Hub may send `poll_data` for immediate updates
3. Both sides may send `heartbeat` for keepalive
4. Error conditions trigger `error` messages

#### 5.1.3 Termination
1. Node sends `disconnect_request` (graceful)
2. Either side closes WebSocket connection
3. Hub cleans up node state and resources

### 5.2 Error Handling

#### 5.2.1 Invalid Magic Number
- **Response**: Immediate connection termination
- **Log**: Security violation logged

#### 5.2.2 Unknown Message Type
- **Response**: `error` message with details
- **Behavior**: Continue processing other messages

#### 5.2.3 Capability Violation
- **Response**: Data filtered silently
- **Log**: Capability violation logged

#### 5.2.4 JSON Parse Errors
- **Response**: `error` message with parse details
- **Behavior**: Continue processing other messages

### 5.3 Timing Requirements

#### 5.3.1 WebSocket Ping/Pong
- **Interval**: Hub sends ping every 30 seconds
- **Timeout**: 5 seconds for pong response
- **Failure**: Connection termination

#### 5.3.2 Message Intervals
- **Minimum**: 100ms between messages
- **Default**: 5000ms subscription interval
- **Maximum**: 3600000ms (1 hour)

#### 5.3.3 Sequence Numbers
- **Initialization**: Start at 1
- **Increment**: +1 for each sent message
- **Wrap**: Allowed at uint32 maximum
- **Validation**: Log out-of-order warnings

## 6. Security Considerations

### 6.1 Authentication
- **Current**: None (local network trust model)
- **Future**: Node authentication via pre-shared keys

### 6.2 Authorization
- **Capability Filtering**: Prevents unauthorized data access
- **Node ID Validation**: Prevents session hijacking

### 6.3 Data Integrity
- **Magic Number**: Prevents accidental connections
- **Sequence Numbers**: Detects replay attacks
- **JSON Validation**: Prevents malformed data injection

### 6.4 Network Security
- **Scope**: Local network only (no internet exposure)
- **Transport**: Unencrypted WebSocket (consider WSS for future)
- **Access Control**: WiFi network access required

## 7. Implementation Guidelines

### 7.1 Node Implementation
- MUST validate magic number in all messages
- MUST implement graceful disconnect on error
- SHOULD implement WebSocket ping/pong handling
- SHOULD buffer data during connection loss
- MAY implement local data storage

### 7.2 Hub Implementation
- MUST validate all message fields
- MUST enforce capability-based filtering
- MUST clean up resources on disconnect
- SHOULD implement connection limits
- MAY implement data persistence

### 7.3 Error Recovery
- Connection loss: Automatic reconnection with exponential backoff
- Parse errors: Continue processing, log errors
- Capability violations: Filter silently, log violations
- Resource exhaustion: Reject new connections gracefully

## 8. Protocol Extensions

### 8.1 Future Message Types
- `actuator_control`: Remote device control
- `firmware_update`: OTA update management
- `configuration`: Dynamic parameter updates
- `alarm`: Event-driven notifications

### 8.2 Capability Extensions
- Additional sensor types (pH, conductivity, etc.)
- Actuator capabilities (pumps, valves, motors)
- Communication features (LoRa, cellular)
- Security features (encryption, certificates)

### 8.3 Transport Extensions
- Binary payload support for efficiency
- Message compression for bandwidth optimization
- WebSocket extensions for enhanced features
- Alternative transports (MQTT, CoAP)

## 9. Compliance and Testing

### 9.1 Reference Implementation
- **Hub**: ESP32-C3 with ESP-IDF v5.5
- **Test Client**: Python asyncio implementation
- **Location**: `test/testnode.py`

### 9.2 Test Scenarios
- Connection establishment and capability negotiation
- Data transmission with various sensor combinations
- Error recovery and graceful disconnection
- Subscription management and filtering
- Concurrent node operation (up to 8 nodes)

### 9.3 Validation Tools
- JSON schema validation for message structure
- Capability mask validation for filtering
- Sequence number validation for ordering
- Timestamp validation for synchronization

## 10. Appendices

### Appendix A: Message Type Reference
```c
// Complete list of message type strings
extern const char MSG_TYP_CONNECT[];              // "connect"
extern const char MSG_TYP_CONNECT_RESPONSE[];     // "connect_response"
extern const char MSG_TYP_NODE_DATA[];            // "node_data"
extern const char MSG_TYP_SUBSCRIBE[];            // "notify_subscription"
extern const char MSG_TYP_POLL_DATA[];            // "poll_data"
extern const char MSG_TYP_OTA_REQUEST[];          // "ota_request"
extern const char MSG_TYP_OTA_STATUS[];           // "ota_status"
extern const char MSG_TYP_DIAGNOSTIC[];           // "diagnostics"
extern const char MSG_TYP_DIAGNOSTIC_REQUEST[];   // "diagnostic_request"
extern const char MSG_TYP_ACK[];                  // "ack"
extern const char MSG_TYP_HEARTBEAT[];            // "heartbeat"
extern const char MSG_TYP_PING[];                 // "ping"
extern const char MSG_TYP_PONG[];                 // "pong"
extern const char MSG_TYP_DISCONNECT_REQUEST[];   // "disconnect_request"
extern const char MSG_TYP_ERROR[];                // "error"
extern const char MSG_TYP_UNKNOWN[];              // "unknown"
```

### Appendix B: Capability Strings
```c
// Sensor capability names
"temperature"    // Temperature sensor
"moisture"       // Soil moisture sensor
"humidity"       // Humidity sensor
"distance"       // Ultrasonic distance sensor
"light"          // Light/LDR sensor

// Service capability names
"diagnostics"    // Diagnostic information
"ota"           // OTA update support
"led"           // LED control
"buzzer"        // Buzzer control
```

### Appendix C: Example Code Fragments

#### C++ Node Implementation (Arduino)
```cpp
void sendConnect() {
    DynamicJsonDocument doc(1024);
    doc["magic"] = 0xBEEFBEEF;
    doc["type"] = "connect";
    doc["node_id"] = nodeId;
    doc["seq_num"] = ++seqNum;
    doc["timestamp"] = time(nullptr);
    doc["controller"] = 2;  // CONTROLLER_ARDUINO
    doc["sw_version"] = "1.0.0";
    
    JsonArray sensors = doc.createNestedArray("sensors");
    sensors.add("temperature");
    sensors.add("humidity");
    
    JsonArray services = doc.createNestedArray("services");
    services.add("diagnostics");
    
    String message;
    serializeJson(doc, message);
    webSocket.sendTXT(message);
}
```

#### Python Node Implementation
```python
def build_connect_message(node_id, seq_num):
    return {
        "magic": 0xBEEFBEEF,
        "type": "connect",
        "node_id": node_id,
        "seq_num": seq_num,
        "timestamp": int(time.time()),
        "controller": 2,
        "sw_version": "1.0.0",
        "sensors": ["temperature", "humidity"],
        "services": ["diagnostics"]
    }
```

---
**Document Version**: 1.0  
**Last Updated**: December 2024  
**Authors**: FloraLink Development Team  
**License**: Project-specific documentation

### Appendix D: Unified LUT (lookup-table) mechanism

Overview
- The project uses compact, compile-time lookup tables (LUTs) for sensors and
    services to map JSON key names to the byte offsets and types inside the
    packed `protocol_msg_t` structures. This keeps parsing code small and
    fast: parsers discover the target field via a string lookup and then copy
    the typed value directly into the correct memory location.

Why a LUT?
- Reduces parsing boilerplate and duplicate code for each field
- Centralizes field metadata (name, capability bit, type, location, offset)
- Enables uniform helper APIs (`nodeio_set_sensor_lut_field`,
    `nodeio_set_service_lut_struct_from_json`, `nodeio_get_sensor_lut_field`)

Risks and safety
- LUTs are low-level and rely on correct offsets. If you change the
    underlying struct layout, the LUT entries must be updated accordingly.
- Incorrect offsets or types can silently corrupt adjacent memory — always
    prefer the provided helpers and avoid manual pointer arithmetic elsewhere.
- LUT names are part of the node↔hub JSON contract. Renaming a key requires
    coordinated updates to node firmware and tests.

Maintenance checklist (recommended)
1. Add field to payload struct(s) (e.g., `sensor_payload_t` or
     `sporadic_sensor_payload_t`).
2. Update the corresponding LUT entry in `main/nodeio_sensors.c` or
     `main/nodeio_services.c` using `offsetof()` for the new offset.
3. If the field is an array, set `elem_count` appropriately and choose
     `FIELD_LOC_PERIODIC` or `FIELD_LOC_SPORADIC` correctly.
4. Add unit tests or testnode scenarios exercising the new key.
5. Update the documentation (this file) and any node implementations.

Helpers to use
- nodeio_find_sensors_lut_field_by_name(name) — lookup LUT entry by JSON key
- nodeio_set_sensor_lut_field(p_msg, payload_index, name, index, src) — safe write
- nodeio_get_sensor_lut_field(p_msg, payload_index, name, index, dst, dst_size) — safe read
- nodeio_set_service_lut_struct_from_json(...) — parse and set structured services
- nodeio_serialize_service_lut(...) — compact service serializer for UI

Developer notes
- Keep LUT entries in alphabetical order where practical to make diffs and
    reviews easier.
- Avoid changing offsets manually; prefer reordering fields in the struct and
    using `offsetof()` to compute offsets.
- Consider adding static assertions or small unit tests that validate the
    expected offsets for critical fields after structural changes.