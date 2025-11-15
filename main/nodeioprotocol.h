#ifndef NODEIOPROTOCOL_H
#define NODEIOPROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// --- Magic number ---
#define PROTOCOL_MAGIC 0xBEEFBEEF

// --- Protocol version ---
#define PROTOCOL_VERSION 1

// --- Protocol features ---
#define PROTOCOL_FEATURE_COMPRESSION (1 << 0)
#define PROTOCOL_FEATURE_ENCRYPTION (1 << 1)

// --- Maximum payload count ---
#define PROTOCOL_MAX_PAYLOAD_COUNT 10

#define NUM_DOOR_SENSORS 8

// --- Capability bitmask ---
typedef enum
{
    CAP_TEMP = 1 << 0,
    CAP_MOISTURE = 1 << 1,
    CAP_HUMIDITY = 1 << 2,
    CAP_DISTANCE = 1 << 3,
    CAP_LIGHTSENSE = 1 << 4,
    CAP_DOORSENSE = 1 << 5, // New capability for door sensors
    CAP_LED = 1 << 6,
    CAP_BUZZER = 1 << 7,
    CAP_DIAG = 1 << 8,
    CAP_OTA = 1 << 9,
    CAP_ALERT = 1 << 10,
    CAP_REMOTE_LOGGING = 1 << 11 // Node supports remote log retrieval
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

    // Network info (from alert code 101)
    char ip_address[16];          // "192.168.68.111"
    int rssi;                     // Signal strength in dBm (e.g., -64)
    char ssid[33];                // WiFi network name
    uint32_t last_network_update; // Timestamp of last network info update

    // Firmware info (from alert code 100)
    char firmware_version[16];     // "v1.0.0"
    char firmware_build_date[32];  // "Nov 9 2025 15:30:45"
    uint32_t last_firmware_update; // Timestamp of last firmware info update

    // Remote logging storage (temporary, cleared after retrieval or timeout)
    char **log_lines;     // Array of log line strings
    uint16_t log_count;   // Number of lines stored
    time_t log_timestamp; // When logs were received
    bool logs_available;  // Flag indicating logs ready for retrieval

    // Lifecycle tracking (from alert codes 102/104)
    char last_wake_source[32];  // "door", "button", "scheduled", etc.
    int expected_uptime_sec;    // Expected active duration (typically 30s)
    int prev_sleep_sec;         // Previous sleep duration
    int prev_uptime_sec;        // Previous active duration
    time_t last_wake_timestamp; // When last wake alert received

    // Anomaly tracking (from alert code 104)
    int last_anomaly_uptime;       // Actual uptime when anomaly detected
    char *last_anomaly_reason;     // What kept node awake (dynamically allocated)
    int last_anomaly_refreshes;    // Activity refresh count
    time_t last_anomaly_timestamp; // When anomaly detected
    uint32_t anomaly_count;        // Total anomalies since connection
} node_params_t;

// --- Protocol message types ---
// Message type values
#define MSG_CONNECT_REQUEST_VAL 0xA0
#define MSG_CONNECT_RESPONSE_VAL 0xA1
#define MSG_NODE_DATA_VAL 0xA2
#define MSG_NODE_EVENT_VAL 0xA3
#define MSG_SUBSCRIBE_VAL 0xA4
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
#define MSG_LOG_REQUEST_VAL 0x11
#define MSG_LOG_RESPONSE_VAL 0x12
#define MSG_UNKNOWN_VAL 0xAF

typedef enum
{
    MSG_CONNECT_REQUEST = 0xA0,
    MSG_CONNECT_RESPONSE = 0xA1,
    MSG_NODE_DATA = 0xA2,
    MSG_NODE_EVENT = 0xA3,
    MSG_SUBSCRIBE = 0xA4,
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
    MSG_LOG_REQUEST = 0x11,
    MSG_LOG_RESPONSE = 0x12,
    MSG_UNKNOWN = 0xAF
} msg_type_t;

// --- JSON Type Strings for the above message types
extern const char MSG_TYP_CONNECT[];
extern const char MSG_TYP_CONNECT_RESPONSE[];
extern const char MSG_TYP_NODE_DATA[];
extern const char MSG_TYP_NODE_EVENT[];
extern const char MSG_TYP_SUBSCRIBE[];
extern const char MSG_TYP_OTA_REQUEST[];
extern const char MSG_TYP_OTA_STATUS[];
extern const char MSG_TYP_DIAGNOSTIC[];
extern const char MSG_TYP_DIAGNOSTIC_REQUEST[];
extern const char MSG_TYP_ACK[];
extern const char MSG_TYP_HEARTBEAT[];
extern const char MSG_TYP_PING[];
extern const char MSG_TYP_PONG[];
extern const char MSG_TYP_DISCONNECT_REQUEST[];
extern const char MSG_TYP_ERROR[];
extern const char MSG_TYP_LOG_REQUEST[];
extern const char MSG_TYP_LOG_RESPONSE[];
extern const char MSG_TYP_UNKNOWN[];

// --- JSON Type Strings for different payload types ---
extern const char MSG_PAYLOAD_TYPE_SENSOR[];
extern const char MSG_PAYLOAD_TYPE_DIAGNOSTIC[];
extern const char MSG_PAYLOAD_TYPE_OTA_STATUS[];
// more to be added

// --- Periodic Sensor Data payload ---
typedef struct
{
    float temp;
    float humidity;
    float distance;
    float moisture;
    float light;
    // Add more as needed
} sensor_payload_t;

typedef enum
{
    EVENT_DOOR,
    EVENT_BUTTON,
    EVENT_MOTION,
    // Add more event types as needed
} sensor_event_type_t;

typedef struct
{
    sensor_event_type_t event_type;
    union
    {
        struct
        {
            uint8_t door_state[NUM_DOOR_SENSORS];
        } door;
        struct
        {
            bool button_pressed;
        } button;
        struct
        {
            bool motion_detected;
        } motion;
        // Add more event structs as needed
    } data;
} sporadic_sensor_payload_t;

// --- OTA/update payload ---
typedef struct
{
    char url[128];
    char version[16];
} ota_request_t;

// --- OTA/update status payload ---
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

// --- Service payload: combines OTA status and diagnostic/health payload ---
typedef struct
{
    ota_status_t ota_status;
    diagnostic_payload_t diagnostics;
    // Add more service payloads as needed
} service_payload_t;

typedef enum
{
    SERVICE_EVENT_ALERT = 0,
    SERVICE_EVENT_OTA = 1,
    // Add more service event types as needed
} service_event_type_t;

typedef struct
{
    service_event_type_t event_type;
    union
    {
        struct
        {
            int alert_code;
            char alert_message[128]; // Alert messages can be up to 128 bytes
        } alert;
        struct
        {
            int ota_status_code;
            char ota_message[64];
        } ota;
        // Add more service event structs as needed
    } data;
} sporadic_service_payload_t;

// Payload type containing union of data type and total number of periodic payload packets
typedef struct
{
    capability_t current_cap_mask; // Indicates which fields in the struct are valid
    union
    {
        sensor_payload_t sensor;
        service_payload_t service;
    } datafields;
} periodic_data_t;

// Payload type containing union of data type and total number of sporadic payload packets
typedef struct
{
    capability_t current_cap_mask; // Indicates which fields in the struct are valid
    union
    {
        sporadic_sensor_payload_t sensor;
        sporadic_service_payload_t service;
    } datafields;
} sporadic_data_t;

typedef struct
{
    uint8_t payload_count; // total number of payload packets
    periodic_data_t periodic_data[PROTOCOL_MAX_PAYLOAD_COUNT];
    sporadic_data_t sporadic_data;
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
