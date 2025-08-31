#ifndef NODEIOPROTOCOL_H
#define NODEIOPROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

// --- Magic number ---
#define PROTOCOL_MAGIC 0xBEEFBEEF

// --- Capability bitmask ---
typedef enum
{
    CAP_MOISTURE = 1 << 0,
    CAP_TEMP = 1 << 1,
    CAP_HUMIDITY = 1 << 2,
    CAP_DISTANCE = 1 << 3,
    CAP_LIGHTSENSE = 1 << 4,
    CAP_LED = 1 << 5,
    CAP_BUZZER = 1 << 6
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
    char node_id[32];
    controller_type_t controller;
    capability_t capability_mask;
    nodeio_state_t current_state;
    char sw_version[16];
} node_params_t;

// --- Protocol message types ---
typedef enum
{
    MSG_CONNECT_REQUEST = 0xA1,
    MSG_CONNECT_RESPONSE = 0xA2,
    MSG_SENSOR_DATA = 0xA3,
    MSG_SUBSCRIBE = 0xA4,
    MSG_POLL_DATA = 0xA5,
    MSG_OTA_REQUEST = 0xA6,
    MSG_OTA_STATUS = 0xA7,
    MSG_DIAGNOSTIC = 0xA8,
    MSG_DIAGNOSTIC_REQUEST = 0xA9,
    MSG_ACK = 0xAA,
    MSG_ERROR = 0xAB
} msg_type_t;

// --- Protocol message envelope ---
typedef struct
{
    uint32_t magic;
    msg_type_t type;
    char node_id[32];
    uint32_t seq_num;
    uint32_t timestamp;
    char payload[256]; // JSON or struct-serialized payload
} protocol_msg_t;

// --- Data payload ---
typedef struct
{
    capability_t capability_mask;
    float temp;
    float humidity;
    float distance;
    float moisture;
    float light;
    // Add more as needed
} node_data_t;

// --- Subscription/config payload ---
typedef struct
{
    capability_t subscribe_mask;
    uint32_t interval_ms;
} subscribe_config_t;

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
} diagnostic_t;

#endif // NODEIOPROTOCOL_H
