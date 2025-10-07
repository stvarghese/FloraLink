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
All messages MUST contain these fields:

```json
{
    "magic": 3203391147,        // Protocol validation (0xBEEFBEEF)
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

#### 2.2.2 Message Type
- **Type**: string
- **Values**: See Section 3 (Message Types)
- **Purpose**: Determines message handling and payload structure
- **Validation**: Unknown types SHOULD trigger error response

#### 2.2.3 Node ID
- **Type**: uint8 (JSON number)
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
```

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
    CAP_OTA = 1 << 8            // OTA update capability
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