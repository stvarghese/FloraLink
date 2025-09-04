#ifndef NODEIOPROTOCOL_H
#define NODEIOPROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// --- Magic number ---
#define PROTOCOL_MAGIC 0xBEEFBEEF

// --- Protocol version ---
#define PROTOCOL_VERSION 1

// --- Protocol features ---
#define PROTOCOL_FEATURE_COMPRESSION (1 << 0)
#define PROTOCOL_FEATURE_ENCRYPTION (1 << 1)

// --- Maximum payload count ---
#define PROTOCOL_MAX_PAYLOAD_COUNT 10

// --- Capability bitmask ---
typedef enum
{
    CAP_TEMP = 1 << 0,
    CAP_MOISTURE = 1 << 1,
    CAP_HUMIDITY = 1 << 2,
    CAP_DISTANCE = 1 << 3,
    CAP_LIGHTSENSE = 1 << 4,
    CAP_LED = 1 << 5,
    CAP_BUZZER = 1 << 6,
    CAP_DIAG = 1 << 7,
    CAP_OTA = 1 << 8
    // max 32 bits
} capability_flag_t;

typedef uint32_t capability_t;

// --- Controller type ---
typedef enum
{
    CONTROLLER_NODEMCU,
    CONTROLLER_ESP32,
    CONTROLLER_ARDUINO,
    CONTROLLER_UNKNOWN
} controller_type_t;

// --- Node connection state ---
typedef enum
{
    NODEIO_STATE_DISCONNECTED,
    NODEIO_STATE_CONNECTING,
    NODEIO_STATE_CONNECTED
} nodeio_state_t;

// --- Node parameters ---
typedef struct
{
    uint8_t node_id;
    controller_type_t controller;
    capability_t capability_mask;
    nodeio_state_t current_state;
    char sw_version[16];
} node_params_t;

// --- Protocol message types ---
// Message type values
#define MSG_CONNECT_REQUEST_VAL 0xA0
#define MSG_CONNECT_RESPONSE_VAL 0xA1
#define MSG_SENSOR_DATA_VAL 0xA2
#define MSG_SUBSCRIBE_VAL 0xA3
#define MSG_POLL_DATA_VAL 0xA4
#define MSG_OTA_REQUEST_VAL 0xA5
#define MSG_OTA_STATUS_VAL 0xA6
#define MSG_DIAGNOSTIC_VAL 0xA7
#define MSG_DIAGNOSTIC_REQUEST_VAL 0xA8
#define MSG_ACK_VAL 0xA9
#define MSG_HEARTBEAT_VAL 0xAA
#define MSG_PING_VAL 0xAB
#define MSG_PONG_VAL 0xAC
#define MSG_DISCONNECT_REQUEST_VAL 0xAD
#define MSG_ERROR_VAL 0xAE
#define MSG_UNKNOWN_VAL 0xAF

typedef enum
{
    MSG_CONNECT_REQUEST = 0xA0,
    MSG_CONNECT_RESPONSE = 0xA1,
    MSG_NODE_DATA = 0xA2,
    MSG_SUBSCRIBE = 0xA3,
    MSG_POLL_DATA = 0xA4,
    MSG_OTA_REQUEST = 0xA5,
    MSG_OTA_STATUS = 0xA6,
    MSG_DIAGNOSTIC = 0xA7,
    MSG_DIAGNOSTIC_REQUEST = 0xA8,
    MSG_ACK = 0xA9,
    MSG_HEARTBEAT = 0xAA,
    MSG_PING = 0xAB,
    MSG_PONG = 0xAC,
    MSG_DISCONNECT_REQUEST = 0xAD,
    MSG_ERROR = 0xAE,
    MSG_UNKNOWN = 0xAF
} msg_type_t;

// --- JSON Type Strings for the above message types
char MSG_TYP_CONNECT[] = "connect";
char MSG_TYP_CONNECT_RESPONSE[] = "connect_response";
char MSG_TYP_NODE_DATA[] = "node_data";
char MSG_TYP_SUBSCRIBE[] = "subscribe";
char MSG_TYP_POLL_DATA[] = "poll_data";
char MSG_TYP_OTA_REQUEST[] = "ota_request";
char MSG_TYP_OTA_STATUS[] = "ota_status";
char MSG_TYP_DIAGNOSTIC[] = "diagnostics";
char MSG_TYP_DIAGNOSTIC_REQUEST[] = "diagnostic_request";
char MSG_TYP_ACK[] = "ack";
char MSG_TYP_HEARTBEAT[] = "heartbeat";
char MSG_TYP_PING[] = "ping";
char MSG_TYP_PONG[] = "pong";
char MSG_TYP_DISCONNECT_REQUEST[] = "disconnect_request";
char MSG_TYP_ERROR[] = "error";
char MSG_TYP_UNKNOWN[] = "unknown";

// --- JSON Type Strings for different payload types ---
char MSG_PAYLOAD_TYPE_SENSOR[] = "sensor";
char MSG_PAYLOAD_TYPE_DIAGNOSTIC[] = "diagnostic";
char MSG_PAYLOAD_TYPE_OTA_STATUS[] = "ota_status";
// more to be added

// --- Sensor Data payload ---
typedef struct
{
    float temp;
    float humidity;
    float distance;
    float moisture;
    float light;
    // Add more as needed
} sensor_payload_t;

// --- OTA/update payload ---
typedef struct
{
    char url[128];
    char version[16];
} ota_request_t;

typedef struct
{
    int status_code;
    char message[64];
} ota_status_t;

// --- Diagnostic/health payload ---
typedef struct
{
    uint32_t uptime_sec;
    uint32_t free_heap;
    int rssi;
    int error_code;
} diagnostic_payload_t;

// Payload type containing union of data type and total number of payload packets
typedef struct
{
    capability_t current_cap_mask; // Indicates which fields in the struct are valid
    union
    {
        sensor_payload_t sensor;
        ota_status_t ota_status;
        diagnostic_payload_t diagnostic;
    } datafields;
} data_t;

typedef struct
{
    uint8_t payload_count; // total number of payload packets
    data_t data[PROTOCOL_MAX_PAYLOAD_COUNT];
} payload_t;

// --- Protocol message envelope ---
typedef struct
{
    uint32_t magic;
    msg_type_t type;
    uint8_t node_id;
    uint32_t seq_num;
    uint32_t timestamp;
    payload_t payload; // JSON or struct-serialized payload
} protocol_msg_t;

// --- Subscription/config payload ---
typedef struct
{
    capability_t subscribe_mask;
    uint32_t interval_ms;
} subscribe_config_t;

#endif // NODEIOPROTOCOL_H
