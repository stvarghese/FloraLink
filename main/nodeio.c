#include "websockserver.h"
#include "nodeio.h"
#include "modemanager.h"
#include "esp_log.h"
#include "nodeioprotocol.h"
#include "nodeio_services.h"
#include "nodeio_sensors.h"
#include "cJSON.h"
#include "commonutils.h"
#include <time.h>
#include <stddef.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Safe JSON append helper: appends formatted text into json buffer at offset with size json_size.
// Returns number of bytes appended on success, or -1 on truncation/error.
static int json_append(char *json, size_t json_size, int *offset, const char *fmt, ...)
{
    if (!json || !offset || *offset < 0 || json_size == 0)
        return -1;
    int rem = (int)(json_size - *offset);
    if (rem <= 0)
        return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json + *offset, rem, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (n >= rem)
    {
        /* indicate truncation */
        *offset += rem - 1;
        return -1;
    }
    *offset += n;
    return n;
}

// Maximum num of nodes shall be equal to the maximum number of sessions
#define MAX_NODES MAX_SESSIONS

static const char *TAG = "nodeio";

// Mutex protecting log access (MSG_LOG_RESPONSE handler and GET handler)
static SemaphoreHandle_t g_logs_mutex = NULL;

typedef struct
{
    node_params_t *p_node;
    protocol_msg_t *p_msg_data;  // Latest periodic/data message
    protocol_msg_t *p_msg_event; // Latest sporadic/event message
    wss_session_t *p_session;
    subscribe_config_t subscription; // Add this line
    bool subscribed;                 // Track if a subscription is active
    bool subscription_update;        // Track if subscription parameters were updated
    int64_t node_uptime_start;       // Timestamp when node connected
    int64_t node_uptime;             // Node uptime in seconds
    int64_t last_uptime_log_ts;      // Timestamp (seconds) when uptime was last logged
} node_context_t;

static node_context_t node_contexts[MAX_NODES] = {0};

typedef struct
{
    const char *name;
    capability_t cap;
    size_t offset;
} sensor_lookup_t;

const sensor_lookup_t sensor_table[] = {
    /* Sensor Name   Capability Mask   Offset within sensor_payload_t */
    {"temperature", CAP_TEMP, offsetof(sensor_payload_t, temp)},
    {"moisture", CAP_MOISTURE, offsetof(sensor_payload_t, moisture)},
    {"humidity", CAP_HUMIDITY, offsetof(sensor_payload_t, humidity)},
    {"distance", CAP_DISTANCE, offsetof(sensor_payload_t, distance)},
    {"light", CAP_LIGHTSENSE, offsetof(sensor_payload_t, light)}
    // Add more sensor types as needed
};

// Unified LUTs for sensors/services are now private to their helper modules

// Service lookup table for dynamic service filter
typedef struct
{
    const char *name;
    capability_t cap;
    size_t offset;
} service_lookup_t;

const service_lookup_t service_table[] = {
    {"diagnostics", CAP_DIAG, offsetof(service_payload_t, diagnostics)},
    {"ota", CAP_OTA, offsetof(service_payload_t, ota_status)}
    // Add more services as needed
};

// Per node local sequence number
static uint32_t node_local_seq[MAX_NODES] = {0};

// Door sensor ownership tracking: door_owner[door_index] = node_id (or -1 if unclaimed)
static int8_t door_owner[NUM_DOOR_SENSORS] = {-1, -1, -1};

// Simple node-to-global door mapping: each node has at most ONE door
// node_door_map[node_id] = global_index (or -1 if node has no door assigned)
static int8_t node_door_map[MAX_NODES];

// Helper function to find next free global door slot
static int find_next_free_door_slot(void)
{
    for (int i = 0; i < NUM_DOOR_SENSORS; i++)
    {
        if (door_owner[i] == -1)
            return i;
    }
    return -1; // All slots taken
}

// Function declarations
static inline msg_type_t nodeio_type_str_to_enum(const char *type_str);
static inline esp_err_t nodeio_parse_message_payload(cJSON *root, capability_t node_cap_mask, protocol_msg_t *p_currentmsg);
static void nodeio_handle_message(int client_fd, const char *data, size_t len);
static void nodeio_send_response(int client_fd, const char *response, size_t len) __attribute__((unused));
static void nodeio_broadcast(const char *message, size_t len) __attribute__((unused));
static void nodeio_subscribe_to_node(int client_fd, const subscribe_config_t *config);
static void nodeio_unsubscribe_from_node(int client_fd);
static void nodeio_request_ota(int client_fd, const ota_request_t *ota) __attribute__((unused));
static void nodeio_report_ota_status(int client_fd, const ota_status_t *status) __attribute__((unused));
static void nodeio_send_connect_response(int client_fd, uint32_t seq_num);
static void nodeio_send_error(int client_fd, const char *error_msg);
static void nodeio_process_diagnostic(int client_fd, const char *diag_info) __attribute__((unused));
static void nodeio_request_diagnostic(int client_fd) __attribute__((unused));
static void nodeio_handle_disconnect(int client_fd, uint8_t node_id);
static node_params_t *nodeio_handle_connect(int client_fd, uint8_t node_id, cJSON *root);
static void nodeio_handle_error(int client_fd, const char *error_msg);
static void nodeio_handle_timeout(int client_fd) __attribute__((unused));
static void nodeio_handle_heartbeat(int client_fd) __attribute__((unused));
static void nodeio_on_close(int client_fd);
static void nodeio_on_message(int client_fd, const char *data, size_t len);

// Ensure service setter prototype is visible (some compile units may not see nodeio_services.h early)
nio_s_err_t nodeio_set_service_lut_struct_from_json(protocol_msg_t *p_msg, size_t payload_index, const char *service_name, struct cJSON *service_obj);

// Helper function to map type_str to msg_type_t
typedef struct
{
    const char *type_str;
    msg_type_t type_enum;
} type_map_t;

static const type_map_t msg_type_map[] = {
    {MSG_TYP_CONNECT, MSG_CONNECT_REQUEST},
    {MSG_TYP_CONNECT_RESPONSE, MSG_CONNECT_RESPONSE},
    {MSG_TYP_NODE_DATA, MSG_NODE_DATA},
    {MSG_TYP_NODE_EVENT, MSG_NODE_EVENT},
    {MSG_TYP_SUBSCRIBE, MSG_SUBSCRIBE},
    {MSG_TYP_OTA_REQUEST, MSG_OTA_REQUEST},
    {MSG_TYP_OTA_STATUS, MSG_OTA_STATUS},
    {MSG_TYP_DIAGNOSTIC, MSG_DIAGNOSTIC},
    {MSG_TYP_DIAGNOSTIC_REQUEST, MSG_DIAGNOSTIC_REQUEST},
    {MSG_TYP_ACK, MSG_ACK},
    // Application-level heartbeat not implemented; WebSocket native PING/PONG used instead
    // {MSG_TYP_HEARTBEAT, MSG_HEARTBEAT},
    // {MSG_TYP_PING, MSG_PING},
    // {MSG_TYP_PONG, MSG_PONG},
    {MSG_TYP_DISCONNECT_REQUEST, MSG_DISCONNECT_REQUEST},
    {MSG_TYP_ERROR, MSG_ERROR},
    {MSG_TYP_LOG_REQUEST, MSG_LOG_REQUEST},
    {MSG_TYP_LOG_RESPONSE, MSG_LOG_RESPONSE},
    {MSG_TYP_UNKNOWN, MSG_UNKNOWN},
};

void nodeio_active_nodes_ping(void)
{
    HEAP_TRACE_START("PING");

    for (int i = 0; i < MAX_NODES; ++i)
    {
        if (node_contexts[i].p_node && node_contexts[i].p_session && node_contexts[i].p_session->connected)
        {
            int client_fd = node_contexts[i].p_session->client_fd;
            if (client_fd != -1)
            {
                // Suppress noisy per-ping debug logging; keep the ping action.
                websockserver_ping(client_fd);
            }
        }
    }

    HEAP_TRACE_END_TIMER(); // Ping allocates WebSocket frames and timers, higher threshold expected
}

static inline msg_type_t nodeio_type_str_to_enum(const char *type_str)
{
    // ESP_LOGD(TAG, "Mapping type string: %s to enum", type_str);
    if (!type_str)
        return MSG_UNKNOWN;
    for (size_t i = 0; i < sizeof(msg_type_map) / sizeof(msg_type_map[0]); ++i)
    {
        if (strcmp(type_str, msg_type_map[i].type_str) == 0)
            return msg_type_map[i].type_enum;
    }
    return MSG_UNKNOWN;
}

// Build/extend capability mask from sensors array
static inline capability_t nodeio_build_node_capmask_sensors(cJSON *sensors, capability_t current_mask)
{
    if (sensors && cJSON_IsArray(sensors))
    {
        int sensor_count = cJSON_GetArraySize(sensors);
        for (int i = 0; i < sensor_count; ++i)
        {
            cJSON *sensor_item = cJSON_GetArrayItem(sensors, i);
            if (sensor_item && cJSON_IsString(sensor_item))
            {
                const char *sensor_name = sensor_item->valuestring;
                const field_lookup_t *s_lut = nodeio_get_sensors_lut();
                size_t s_count = nodeio_get_sensors_lut_count();
                for (size_t s = 0; s < s_count; ++s)
                {
                    /* Accept legacy alias 'door_state' as equivalent to LUT name 'doorsense' */
                    if (strcmp(sensor_name, s_lut[s].name) == 0 ||
                        (strcmp(sensor_name, "door_state") == 0 && strcmp(s_lut[s].name, "doorsense") == 0))
                    {
                        current_mask |= s_lut[s].cap;
                        break;
                    }
                }
            }
        }
    }
    return current_mask;
}

// Build/extend capability mask from services array
static inline capability_t nodeio_build_node_capmask_services(cJSON *services_array, capability_t current_mask)
{
    if (services_array && cJSON_IsArray(services_array))
    {
        int service_count = cJSON_GetArraySize(services_array);
        for (int i = 0; i < service_count; ++i)
        {
            cJSON *service_item = cJSON_GetArrayItem(services_array, i);
            if (service_item && cJSON_IsString(service_item))
            {
                const char *service_name = service_item->valuestring;
                const field_lookup_t *sv_lut = nodeio_get_services_lut();
                size_t sv_count = nodeio_get_services_lut_count();
                for (size_t s = 0; s < sv_count; ++s)
                {
                    if (strcmp(service_name, sv_lut[s].name) == 0)
                    {
                        current_mask |= sv_lut[s].cap;
                        break;
                    }
                }
            }
        }
    }
    return current_mask;
}

// Helper to update node parameters from JSON
static inline node_params_t *nodeio_update_node_params_from_json(uint8_t node_id, cJSON *root)
{
    HEAP_TRACE_START("UPDATE_PARAMS");

    if (!root)
    {
        ESP_LOGE(TAG, "Invalid JSON root");
        HEAP_TRACE_END_DEFAULT();
        return NULL;
    }
    // Only single pointer needed because we're just updating the structure fields
    node_params_t *node = node_contexts[node_id].p_node;
    if (!node)
    {
        ESP_LOGE(TAG, "Node pointer not assigned for this node id");
        return NULL;
    }

    // node id, should be the same as node context array index
    node->node_id = node_id;
    node->controller = cJSON_GetObjectItem(root, "controller") ? (controller_type_t)cJSON_GetObjectItem(root, "controller")->valueint : CONTROLLER_UNKNOWN;
    node->capability_mask |= nodeio_build_node_capmask_sensors(cJSON_GetObjectItem(root, "sensors"), node->capability_mask);
    node->capability_mask |= nodeio_build_node_capmask_services(cJSON_GetObjectItem(root, "services"), node->capability_mask);
    ESP_LOGD(TAG, "Node %d capability mask: 0x%08X", node_id, node->capability_mask);
    node->current_state = NODEIO_STATE_CONNECTED;
    cJSON *sw_version_item = cJSON_GetObjectItem(root, "sw_version");
    if (sw_version_item && cJSON_IsString(sw_version_item))
    {
        strncpy(node->sw_version, sw_version_item->valuestring, sizeof(node->sw_version) - 1);
        node->sw_version[sizeof(node->sw_version) - 1] = '\0';
    }

    HEAP_TRACE_END_DEFAULT();
    return node;
}

// Helper function to parse payload from JSON
// Forward helpers
static inline esp_err_t nodeio_parse_periodic_payload_item(protocol_msg_t *p_msg, cJSON *item, size_t payload_index, capability_t node_cap_mask);
static inline esp_err_t nodeio_parse_sporadic_event(protocol_msg_t *p_msg, cJSON *msg_payload, capability_t node_cap_mask);

static inline esp_err_t nodeio_parse_message_payload(cJSON *root, capability_t node_cap_mask, protocol_msg_t *p_currentmsg)
{
    HEAP_TRACE_START("PARSE_PAYLOAD");

    // Check pointers
    if (!root || !p_currentmsg)
    {
        ESP_LOGE(TAG, "Invalid JSON or message structure");
        return ESP_FAIL;
    }
    if (node_cap_mask == 0)
    {
        ESP_LOGW(TAG, "Node capability mask is empty");
    }

    cJSON *msg_payload = cJSON_GetObjectItem(root, "payload");
    // For node_data, payload is an array; for node_event, payload is an object
    if (msg_payload && cJSON_IsArray(msg_payload))
    {
        int count = cJSON_GetArraySize(msg_payload);
        p_currentmsg->payload.payload_count = (count > PROTOCOL_MAX_PAYLOAD_COUNT) ? PROTOCOL_MAX_PAYLOAD_COUNT : count;
        for (int i = 0; i < p_currentmsg->payload.payload_count; ++i)
        {
            cJSON *item = cJSON_GetArrayItem(msg_payload, i);
            if (nodeio_parse_periodic_payload_item(p_currentmsg, item, i, node_cap_mask) != ESP_OK)
            {
                ESP_LOGW(TAG, "Failed to parse periodic payload item at index %d", i);
            }
        }

        HEAP_TRACE_END_DEFAULT();
        return ESP_OK;
    }
    // Node event: payload is an object, expecting sporadic_data_t - event triggered
    else if (msg_payload && cJSON_IsObject(msg_payload))
    {
        // Delegate sporadic/event parsing to helper which writes only to sporadic_data
        if (nodeio_parse_sporadic_event(p_currentmsg, msg_payload, node_cap_mask) != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to parse sporadic event payload");
            HEAP_TRACE_END_DEFAULT();
            return ESP_FAIL;
        }
        HEAP_TRACE_END_DEFAULT();
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "No valid payload found in message");
        HEAP_TRACE_END_DEFAULT();
        return ESP_FAIL;
    }
}

// Parse a single periodic payload array item (sensor or service) into p_msg->payload.periodic_data[payload_index]
static inline esp_err_t nodeio_parse_periodic_payload_item(protocol_msg_t *p_msg, cJSON *item, size_t payload_index, capability_t node_cap_mask)
{
    if (!p_msg || !item)
        return ESP_FAIL;

    cJSON *type_item = cJSON_GetObjectItem(item, "type");
    if (!type_item || !cJSON_IsString(type_item))
        return ESP_FAIL;

    const char *t = type_item->valuestring;
    if (strcmp(t, "sensor") == 0)
    {
        cJSON *sensor_obj = cJSON_GetObjectItem(item, "sensor");
        if (!sensor_obj || !cJSON_IsObject(sensor_obj))
            return ESP_FAIL;

        cJSON *field = sensor_obj->child;
        while (field)
        {
            const char *key = field->string;
            if (!key)
            {
                field = field->next;
                continue;
            }
            // find in sensors LUT
            const field_lookup_t *fld = nodeio_find_sensors_lut_field_by_name(key);
            if (!fld)
            {
                ESP_LOGW(TAG, "Unknown sensor key '%s' in periodic payload", key);
                field = field->next;
                continue;
            }
            // ensure capability is present
            if (!(node_cap_mask & fld->cap))
            {
                ESP_LOGD(TAG, "Sensor '%s' capability not present, ignoring", key);
                field = field->next;
                continue;
            }
            // Only handle simple float and scalar cases here
            if (fld->ftype == FIELD_TYPE_FLOAT && cJSON_IsNumber(field))
            {
                float v = (float)field->valuedouble;
                if (nodeio_set_sensor_lut_field(p_msg, payload_index, key, 0, &v) != NIO_OK)
                {
                    ESP_LOGW(TAG, "Failed to set sensor field '%s' into periodic slot %u", key, (unsigned)payload_index);
                }
            }
            else
            {
                ESP_LOGW(TAG, "Unsupported sensor field type or JSON type for '%s'", key);
            }
            field = field->next;
        }
        return ESP_OK;
    }
    else if (strcmp(t, "ota_status") == 0)
    {
        cJSON *ota_obj = cJSON_GetObjectItem(item, "ota_status");
        if (ota_obj && cJSON_IsObject(ota_obj))
        {
            return (nodeio_set_service_lut_struct_from_json(p_msg, payload_index, "ota_status", ota_obj) == NIO_S_OK) ? ESP_OK : ESP_FAIL;
        }
    }
    else if (strcmp(t, "diagnostics") == 0)
    {
        cJSON *diag_obj = cJSON_GetObjectItem(item, "diagnostics");
        if (diag_obj && cJSON_IsObject(diag_obj))
        {
            return (nodeio_set_service_lut_struct_from_json(p_msg, payload_index, "diagnostics", diag_obj) == NIO_S_OK) ? ESP_OK : ESP_FAIL;
        }
    }
    return ESP_FAIL;
}

// Parse sporadic/event payload (object form) into p_msg->payload.sporadic_data only
static inline esp_err_t nodeio_parse_sporadic_event(protocol_msg_t *p_msg, cJSON *msg_payload, capability_t node_cap_mask)
{
    if (!p_msg || !msg_payload)
        return ESP_FAIL;

    // Check for service alert format (alert_code + alert_message without event_type)
    // Node may send alerts directly without wrapping in event_type
    cJSON *alert_code_item = cJSON_GetObjectItem(msg_payload, "alert_code");
    cJSON *alert_msg_item = cJSON_GetObjectItem(msg_payload, "alert_message");
    if (alert_code_item && alert_msg_item && cJSON_IsNumber(alert_code_item) && cJSON_IsString(alert_msg_item))
    {
        // Direct alert format - parse via services LUT
        return nodeio_set_service_lut_struct_from_json(p_msg, 0, "alert", msg_payload);
    }

    cJSON *event_type_item = cJSON_GetObjectItem(msg_payload, "event_type");
    if (!event_type_item || !cJSON_IsString(event_type_item))
        return ESP_FAIL;

    const char *etype = event_type_item->valuestring;
    if (strcmp(etype, "EVENT_DOOR") == 0)
    {
        /* Use the sensors LUT to validate the field and element count, and to
           drive the write into sporadic storage via nodeio_set_sensor_lut_field(). */
        const field_lookup_t *fld = nodeio_find_sensors_lut_field_by_name("doorsense");
        if (!fld)
        {
            ESP_LOGW(TAG, "No LUT entry for sporadic sensor 'doorsense'");
            return ESP_FAIL;
        }

        /* If node doesn't advertise capability, ignore the event silently. */
        if (!(node_cap_mask & fld->cap))
        {
            ESP_LOGD(TAG, "Node capability does not include 'doorsense', ignoring event");
            return ESP_OK;
        }

        /* Hub subscription check removed: nodes only send events they were asked
           to send. Acceptance is based on node capability (checked above). */

        if (fld->loc != FIELD_LOC_SPORADIC)
        {
            ESP_LOGW(TAG, "LUT entry for 'doorsense' not marked sporadic, refusing to write into sporadic area");
            return ESP_FAIL;
        }

        /* Support two format options:
           1) Single-door implicit: { "state": "OPEN" } - Simple, practical (one door per node)
           2) Legacy per-index keys: { "doorsense_0": "OPEN" } - Backward compatibility
        */

        // Check for single-door format first (preferred)
        cJSON *state_item = cJSON_GetObjectItem(msg_payload, "state");
        if (state_item && cJSON_IsString(state_item))
        {
            // Single-door format: one door per node
            uint8_t val = 0xFF;
            if (strcmp(state_item->valuestring, "OPEN") == 0)
                val = 1;
            else if (strcmp(state_item->valuestring, "CLOSED") == 0)
                val = 0;

            // Look up or auto-assign global index
            int global_idx = node_door_map[p_msg->node_id];
            if (global_idx == -1)
            {
                // First time: try node_id as default global index for convenience
                global_idx = p_msg->node_id;
                if (global_idx >= NUM_DOOR_SENSORS || door_owner[global_idx] != -1)
                {
                    // node_id slot taken or out of range, find next free slot
                    global_idx = find_next_free_door_slot();
                    if (global_idx == -1)
                    {
                        ESP_LOGW(TAG, "Node %d: no free global door slots available", p_msg->node_id);
                        return ESP_FAIL;
                    }
                }
                // Assign mapping and claim ownership
                node_door_map[p_msg->node_id] = global_idx;
                door_owner[global_idx] = p_msg->node_id;
                ESP_LOGI(TAG, "Node %d door → global door %d", p_msg->node_id, global_idx);
            }
            else
            {
                // Mapping exists, verify ownership
                if (door_owner[global_idx] != p_msg->node_id)
                {
                    ESP_LOGW(TAG, "Node %d door mapped to global %d, but owned by node %d - REJECTED",
                             p_msg->node_id, global_idx, door_owner[global_idx]);
                    return ESP_FAIL;
                }
            }

            // Store the value (ownership already verified)
            if (nodeio_set_sensor_lut_field(p_msg, 0, "doorsense", (size_t)global_idx, &val) != NIO_OK)
            {
                ESP_LOGW(TAG, "Failed to set sporadic doorsense_%d", global_idx);
                return ESP_FAIL;
            }

            const char *sval = (val == 1) ? "OPEN" : (val == 0) ? "CLOSED"
                                                                : "UNKNOWN";
            ESP_LOGI(TAG, "Node %d: door = %s (global slot %d)", p_msg->node_id, sval, global_idx);
            return ESP_OK;
        }

        /* Legacy per-index format: "doorsense_N": "OPEN"
         * Since we now do single-door-per-node, we treat any doorsense_N key
         * as the node's single door, ignoring the N suffix. Just look for first match. */

        cJSON *current_item = msg_payload->child;
        while (current_item)
        {
            if (!current_item->string)
            {
                current_item = current_item->next;
                continue;
            }

            // Check if this is a doorsense_N or door_state_N key
            if (strncmp(current_item->string, "doorsense_", 10) == 0 ||
                strncmp(current_item->string, "door_state_", 11) == 0)
            {
                // Parse the value (string or number)
                uint8_t val = 0xFF;
                if (cJSON_IsString(current_item))
                {
                    if (strcmp(current_item->valuestring, "OPEN") == 0)
                        val = 1;
                    else if (strcmp(current_item->valuestring, "CLOSED") == 0)
                        val = 0;
                }
                else if (cJSON_IsNumber(current_item))
                {
                    val = (uint8_t)current_item->valueint;
                }

                if (val == 0xFF)
                {
                    current_item = current_item->next;
                    continue; // Skip unknown values
                }

                // Use same auto-assignment logic as new single-door format
                int global_idx = node_door_map[p_msg->node_id];
                if (global_idx == -1)
                {
                    // First door event from this node, auto-assign
                    global_idx = p_msg->node_id;
                    if (global_idx >= NUM_DOOR_SENSORS || door_owner[global_idx] != -1)
                    {
                        global_idx = find_next_free_door_slot();
                        if (global_idx == -1)
                        {
                            ESP_LOGW(TAG, "Node %d: no free global door slots available (legacy format)", p_msg->node_id);
                            return ESP_FAIL;
                        }
                    }
                    node_door_map[p_msg->node_id] = global_idx;
                    door_owner[global_idx] = p_msg->node_id;
                    ESP_LOGI(TAG, "Node %d door → global door %d (legacy format)", p_msg->node_id, global_idx);
                }

                // Store the value
                if (nodeio_set_sensor_lut_field(p_msg, 0, "doorsense", (size_t)global_idx, &val) != NIO_OK)
                {
                    ESP_LOGW(TAG, "Failed to set sporadic doorsense_%d", global_idx);
                    return ESP_FAIL;
                }

                const char *sval = (val == 1) ? "OPEN" : (val == 0) ? "CLOSED"
                                                                    : "UNKNOWN";
                ESP_LOGI(TAG, "Node %d: door = %s (global slot %d, legacy format)", p_msg->node_id, sval, global_idx);
                return ESP_OK; // Successfully processed first door key found
            }

            current_item = current_item->next;
        }

        // No door keys found
        ESP_LOGW(TAG, "Door event payload has no recognizable door state fields");
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "Unknown event_type: %s", etype);
        return ESP_FAIL;
    }
}

static void nodeio_handle_message(int client_fd, const char *data, size_t len)
{
    HEAP_TRACE_START("NODEIO");
    // bool is_valid = false;

    // ESP_LOGD(TAG, "Received message: %s, length: %u", data, len);
    // // 1. Copy and null-terminate the data
    // char msg[512];
    // size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    // memcpy(msg, data, copy_len);
    // msg[copy_len] = '\0';

    // 2. Parse the message as JSON and map to protocol types defined in nodeioprotocol.h
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON");
        nodeio_handle_error(client_fd, "Invalid JSON");
        return;
    }

    // Extract magic (as per nodeioprotocol.h)
    cJSON *magic_item = cJSON_GetObjectItem(root, "magic");
    uint32_t magic = 0;
    if (magic_item && cJSON_IsNumber(magic_item))
    {
        magic = (uint32_t)cJSON_GetNumberValue(magic_item);
    }
    if (magic != PROTOCOL_MAGIC)
    {
        ESP_LOGW(TAG, "Invalid protocol magic number: 0x%08X, expected: 0x%08X", magic, PROTOCOL_MAGIC);
        nodeio_handle_error(client_fd, "Invalid protocol magic");
        cJSON_Delete(root);
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    cJSON *node_id_item = cJSON_GetObjectItem(root, "node_id");
    cJSON *seq_num_item = cJSON_GetObjectItem(root, "seq_num");
    cJSON *timestamp_item = cJSON_GetObjectItem(root, "timestamp");

    // If any of the required fields are missing, handle the error
    if (!type_item || !cJSON_IsString(type_item) ||
        !node_id_item || !cJSON_IsNumber(node_id_item) ||
        !seq_num_item || !cJSON_IsNumber(seq_num_item) ||
        !timestamp_item || !cJSON_IsNumber(timestamp_item))
    {
        // log only the missing item
        if (!type_item || !cJSON_IsString(type_item))
        {
            ESP_LOGW(TAG, "Missing or invalid 'type' field");
        }
        if (!node_id_item || !cJSON_IsNumber(node_id_item))
        {
            ESP_LOGW(TAG, "Missing or invalid 'node_id' field");
        }
        if (!seq_num_item || !cJSON_IsNumber(seq_num_item))
        {
            ESP_LOGW(TAG, "Missing or invalid 'seq_num' field");
        }
        if (!timestamp_item || !cJSON_IsNumber(timestamp_item))
        {
            ESP_LOGW(TAG, "Missing or invalid 'timestamp' field");
        }

        nodeio_handle_error(client_fd, "Missing or invalid fields");
        cJSON_Delete(root);
        return;
    }

    // Parse msg_type, node_id, seq_num, timestamp
    const char *type_str = type_item->valuestring;
    int node_id = node_id_item && cJSON_IsNumber(node_id_item) ? node_id_item->valueint : -1;
    uint32_t seq_num = seq_num_item && cJSON_IsNumber(seq_num_item) ? seq_num_item->valueint : 0;
    uint32_t timestamp = timestamp_item && cJSON_IsNumber(timestamp_item) ? timestamp_item->valueint : 0;

    // node_id shall be less than MAX_NODES
    if (node_id < 0 || node_id >= MAX_NODES)
    {
        nodeio_handle_error(client_fd, "Invalid node_id");
        cJSON_Delete(root);
        return;
    }

    // Get node context (may or may not be NULL)
    node_params_t **const pp_node = &node_contexts[node_id].p_node;
    wss_session_t **const pp_session = &node_contexts[node_id].p_session;

    // Free any existing message for this node before allocating new one
    /* Do not free other message slot here; we'll assign the new message to the
       appropriate per-type slot after successful parsing to avoid premature
       freeing. */

    // Convert type_str to enum msg_type_t and validate before allocating message buffer
    msg_type_t msg_type = nodeio_type_str_to_enum(type_str);
    ESP_LOGD(TAG, "Message type: %s, 0x%02X", type_str, msg_type);
    // Check if the message type is valid
    if (msg_type == MSG_UNKNOWN)
    {
        nodeio_handle_error(client_fd, "Unknown message type");
        cJSON_Delete(root);
        return;
    }

    // For new, valid conn requests, check and create a new node context
    if (msg_type == MSG_CONNECT_REQUEST)
    {
        ESP_LOGD(TAG, "Connect message from node_id %d: %s", node_id, cJSON_PrintUnformatted(root));
        if (NULL == nodeio_handle_connect(client_fd, node_id, root))
        {
            nodeio_handle_error(client_fd, "Failed to connect");
            cJSON_Delete(root);
            return;
        }
        else
        {
            // Parse message payload
            if (NULL != nodeio_update_node_params_from_json(node_id, root))
            {
                // Check if node advertises door sense capability and all doors are already owned
                node_params_t *node = node_contexts[node_id].p_node;
                if (node && (node->capability_mask & CAP_DOORSENSE))
                {
                    bool all_doors_owned = true;
                    for (int i = 0; i < NUM_DOOR_SENSORS; i++)
                    {
                        if (door_owner[i] == -1 || door_owner[i] == node_id)
                        {
                            all_doors_owned = false;
                            break;
                        }
                    }
                    if (all_doors_owned)
                    {
                        ESP_LOGW(TAG, "Node %d connected with door sense capability, but all door sensors are already owned by other nodes - door events will be rejected", node_id);
                    }
                }
                // Record the node uptime start time
                node_contexts[node_id].node_uptime_start = esp_timer_get_time() / 1000000; // in seconds
                // Reset node uptime
                node_contexts[node_id].node_uptime = 0;
                // Reset last uptime log timestamp so we will log promptly after connect
                node_contexts[node_id].last_uptime_log_ts = 0;
                // Successfully connected, send response
                nodeio_send_connect_response(client_fd, seq_num);
                // Trigger initial subscription update
                node_contexts[node_id].subscription_update = true;
                // Wakeup on valid connect request
                modemanager_notify_activity_auto();
            }
            cJSON_Delete(root);
            return;
        }
    }
    else if ((*pp_node == NULL) && (msg_type != MSG_CONNECT_REQUEST))
    {
        nodeio_handle_error(client_fd, "Node not connected or unknown");
        cJSON_Delete(root);
        return;
    }
    else if (msg_type == MSG_DISCONNECT_REQUEST)
    {
        // Handle disconnect request
        nodeio_handle_disconnect(client_fd, node_id);
        cJSON_Delete(root);
        return;
    }

    // If msg_type DATA, process the data message after checking states
    if (msg_type == MSG_NODE_DATA)
    {
        if (NULL == *pp_session || NULL == *pp_node)
        {
            nodeio_handle_error(client_fd, "Session or Node context non existent");
            cJSON_Delete(root);
            return;
        }
        if ((*pp_session)->client_fd == client_fd && (*pp_node)->current_state == NODEIO_STATE_CONNECTED)
        {
            // Allocate a new protocol_msg_t for this message (transient) and populate envelope
            protocol_msg_t *p_currentmsg = calloc(1, sizeof(protocol_msg_t));
            if (!p_currentmsg)
            {
                nodeio_handle_error(client_fd, "Failed to allocate memory");
                cJSON_Delete(root);
                return;
            }
            p_currentmsg->magic = magic_item->valueint;
            p_currentmsg->node_id = node_id;
            p_currentmsg->seq_num = seq_num;
            p_currentmsg->timestamp = timestamp;
            p_currentmsg->type = msg_type;

            // Parse message payload into the transient p_currentmsg
            if (nodeio_parse_message_payload(root, (*pp_node)->capability_mask, p_currentmsg) != ESP_OK)
            {
                nodeio_handle_error(client_fd, "Failed to parse message payload");
                free(p_currentmsg);
                cJSON_Delete(root);
                return;
            }
            // Parsing succeeded: store into the periodic slot, replacing any previous
            if (node_contexts[node_id].p_msg_data)
            {
                free(node_contexts[node_id].p_msg_data);
                node_contexts[node_id].p_msg_data = NULL;
            }
            node_contexts[node_id].p_msg_data = p_currentmsg;
        }
    }

    // If msg_type EVENT, process the event message after checking states
    if (msg_type == MSG_NODE_EVENT)
    {
        if (NULL == *pp_session || NULL == *pp_node)
        {
            nodeio_handle_error(client_fd, "Session or Node context non existent");
            cJSON_Delete(root);
            return;
        }
        if ((*pp_session)->client_fd == client_fd && (*pp_node)->current_state == NODEIO_STATE_CONNECTED)
        {
            // Allocate transient message for event and populate envelope
            protocol_msg_t *p_currentmsg = calloc(1, sizeof(protocol_msg_t));
            if (!p_currentmsg)
            {
                nodeio_handle_error(client_fd, "Failed to allocate memory");
                cJSON_Delete(root);
                return;
            }
            p_currentmsg->magic = magic_item->valueint;
            p_currentmsg->node_id = node_id;
            p_currentmsg->seq_num = seq_num;
            p_currentmsg->timestamp = timestamp;
            p_currentmsg->type = msg_type;

            // Parse message payload into transient p_currentmsg
            if (nodeio_parse_message_payload(root, (*pp_node)->capability_mask, p_currentmsg) != ESP_OK)
            {
                nodeio_handle_error(client_fd, "Failed to parse message payload");
                free(p_currentmsg);
                cJSON_Delete(root);
                return;
            }
            // Parsing succeeded: store into the event slot, replacing any previous
            if (node_contexts[node_id].p_msg_event)
            {
                free(node_contexts[node_id].p_msg_event);
                node_contexts[node_id].p_msg_event = NULL;
            }
            node_contexts[node_id].p_msg_event = p_currentmsg;

            // Check if this is an alert event and handle system alerts (codes 100-199)
            if (p_currentmsg->payload.sporadic_data.current_cap_mask & CAP_ALERT)
            {
                int alert_code = p_currentmsg->payload.sporadic_data.datafields.service.data.alert.alert_code;
                const char *alert_msg = p_currentmsg->payload.sporadic_data.datafields.service.data.alert.alert_message;

                // Handle system alerts (100-199) - these provide node metadata
                if (alert_code == 100)
                {
                    // Firmware info: "FW:v1.0.0 Build:Nov 9 2025 15:30:45"
                    char fw_version[16] = {0};
                    char fw_build[32] = {0};
                    if (sscanf(alert_msg, "FW:%15s Build:%31[^\n]", fw_version, fw_build) == 2)
                    {
                        strncpy((*pp_node)->firmware_version, fw_version, sizeof((*pp_node)->firmware_version) - 1);
                        strncpy((*pp_node)->firmware_build_date, fw_build, sizeof((*pp_node)->firmware_build_date) - 1);
                        (*pp_node)->last_firmware_update = (uint32_t)time(NULL);
                        ESP_LOGI(TAG, "Node %d firmware: %s (Built: %s)", node_id, fw_version, fw_build);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Node %d firmware info format error: %s", node_id, alert_msg);
                    }
                }
                else if (alert_code == 101)
                {
                    // Network status: "IP:192.168.68.111 RSSI:-64 SSID:MyWiFiNetwork"
                    char ip[16] = {0};
                    int rssi = 0;
                    char ssid[33] = {0};
                    // Use a more flexible parser that captures everything after "SSID:"
                    int matched = sscanf(alert_msg, "IP:%15s RSSI:%d SSID:%32[^\n]", ip, &rssi, ssid);
                    if (matched >= 3)
                    {
                        strncpy((*pp_node)->ip_address, ip, sizeof((*pp_node)->ip_address) - 1);
                        (*pp_node)->rssi = rssi;
                        strncpy((*pp_node)->ssid, ssid, sizeof((*pp_node)->ssid) - 1);
                        (*pp_node)->last_network_update = (uint32_t)time(NULL);
                        ESP_LOGI(TAG, "Node %d network: IP=%s, RSSI=%d dBm, SSID=%s", node_id, ip, rssi, ssid);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Node %d network info format error: %s", node_id, alert_msg);
                    }
                }
                else if (alert_code == 102)
                {
                    // Wake event: "WAKE:door EXPECTED_UP:30s PREV_SLEEP:120s PREV_UP:15s"
                    char wake_source[32] = {0};
                    int expected_up = 0, prev_sleep = 0, prev_up = 0;

                    int matched = sscanf(alert_msg, "WAKE:%31s EXPECTED_UP:%ds PREV_SLEEP:%ds PREV_UP:%ds",
                                         wake_source, &expected_up, &prev_sleep, &prev_up);
                    if (matched == 4)
                    {
                        strncpy((*pp_node)->last_wake_source, wake_source, sizeof((*pp_node)->last_wake_source) - 1);
                        (*pp_node)->expected_uptime_sec = expected_up;
                        (*pp_node)->prev_sleep_sec = prev_sleep;
                        (*pp_node)->prev_uptime_sec = prev_up;
                        (*pp_node)->last_wake_timestamp = time(NULL);

                        ESP_LOGI(TAG, "Node %d wake: source=%s expected_up=%ds prev_sleep=%ds prev_up=%ds",
                                 node_id, wake_source, expected_up, prev_sleep, prev_up);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Node %d wake event format error: %s", node_id, alert_msg);
                    }
                }
                else if (alert_code == 104)
                {
                    // Anomaly: "ANOMALY:extended_active UP:65s EXPECTED:30s REASON:door_event REFRESHES:47"
                    int actual_up = 0, expected = 0, refreshes = 0;
                    char reason[32] = {0};

                    int matched = sscanf(alert_msg, "ANOMALY:extended_active UP:%ds EXPECTED:%ds REASON:%31s REFRESHES:%d",
                                         &actual_up, &expected, reason, &refreshes);
                    if (matched == 4)
                    {
                        (*pp_node)->last_anomaly_uptime = actual_up;

                        // Free previous reason string if exists
                        if ((*pp_node)->last_anomaly_reason)
                        {
                            free((*pp_node)->last_anomaly_reason);
                        }
                        (*pp_node)->last_anomaly_reason = strdup(reason);

                        (*pp_node)->last_anomaly_refreshes = refreshes;
                        (*pp_node)->last_anomaly_timestamp = time(NULL);
                        (*pp_node)->anomaly_count++;

                        ESP_LOGW(TAG, "Node %d ANOMALY: active=%ds expected=%ds reason=%s refreshes=%d (total anomalies: %d)",
                                 node_id, actual_up, expected, reason, refreshes, (*pp_node)->anomaly_count);

                        // High refresh count indicates serious hardware issue
                        if (refreshes > 20)
                        {
                            ESP_LOGE(TAG, "Node %d CRITICAL: High refresh count %d suggests hardware issue (%s)",
                                     node_id, refreshes, reason);
                        }
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Node %d anomaly format error: %s", node_id, alert_msg);
                    }
                }
                else if (alert_code >= 100 && alert_code < 200)
                {
                    // Other system alerts (reserved for future use)
                    ESP_LOGI(TAG, "Node %d system alert [%d]: %s", node_id, alert_code, alert_msg);
                }
                else if (alert_code >= 300)
                {
                    // User-defined application alerts
                    ESP_LOGI(TAG, "Node %d alert [%d]: %s", node_id, alert_code, alert_msg);
                }
            }
        }
    }

    // Handle MSG_LOG_RESPONSE
    if (msg_type == MSG_LOG_RESPONSE)
    {
        if (NULL == *pp_session || NULL == *pp_node)
        {
            nodeio_handle_error(client_fd, "Session or Node context non existent");
            cJSON_Delete(root);
            return;
        }
        if ((*pp_session)->client_fd == client_fd && (*pp_node)->current_state == NODEIO_STATE_CONNECTED)
        {
            ESP_LOGD(TAG, "Received log response from node %d", node_id);

            // Extract payload
            cJSON *payload = cJSON_GetObjectItem(root, "payload");
            if (!payload || !cJSON_IsObject(payload))
            {
                ESP_LOGW(TAG, "Log response missing or invalid payload");
                cJSON_Delete(root);
                return;
            }

            cJSON *total_lines_item = cJSON_GetObjectItem(payload, "total_lines");
            cJSON *logs_array = cJSON_GetObjectItem(payload, "logs");

            if (!total_lines_item || !cJSON_IsNumber(total_lines_item) ||
                !logs_array || !cJSON_IsArray(logs_array))
            {
                ESP_LOGW(TAG, "Log response payload missing total_lines or logs array");
                cJSON_Delete(root);
                return;
            }

            int total_lines = total_lines_item->valueint;
            int logs_count = cJSON_GetArraySize(logs_array);

            ESP_LOGD(TAG, "Node %d log response: total_lines=%d, received=%d lines",
                     node_id, total_lines, logs_count);

            // Acquire lock before modifying logs
            nodeio_lock_logs();

            // Free any existing stored logs
            if ((*pp_node)->log_lines)
            {
                for (int i = 0; i < (*pp_node)->log_count; i++)
                {
                    free((*pp_node)->log_lines[i]);
                }
                free((*pp_node)->log_lines);
                (*pp_node)->log_lines = NULL;
            }

            // Store logs for frontend retrieval
            if (logs_count > 0)
            {
                (*pp_node)->log_lines = (char **)malloc(logs_count * sizeof(char *));
                if ((*pp_node)->log_lines)
                {
                    (*pp_node)->log_count = 0;
                    for (int i = 0; i < logs_count; i++)
                    {
                        cJSON *log_item = cJSON_GetArrayItem(logs_array, i);
                        if (log_item && cJSON_IsObject(log_item))
                        {
                            cJSON *line_item = cJSON_GetObjectItem(log_item, "line");
                            if (line_item && cJSON_IsString(line_item))
                            {
                                // Validate and truncate log line to max length
                                const char *src = line_item->valuestring;
                                size_t src_len = strlen(src);
                                if (src_len > 1024) // Max 1KB per line
                                {
                                    ESP_LOGW(TAG, "Log line too long (%zu bytes), truncating", src_len);
                                    src_len = 1024;
                                }
                                (*pp_node)->log_lines[(*pp_node)->log_count] = malloc(src_len + 1);
                                if ((*pp_node)->log_lines[(*pp_node)->log_count])
                                {
                                    strncpy((*pp_node)->log_lines[(*pp_node)->log_count], src, src_len);
                                    (*pp_node)->log_lines[(*pp_node)->log_count][src_len] = '\0';
                                    (*pp_node)->log_count++;
                                }
                                else
                                {
                                    ESP_LOGW(TAG, "Failed to allocate memory for log line %d", i);
                                }
                            }
                        }
                    }
                    (*pp_node)->log_timestamp = time(NULL);
                    (*pp_node)->logs_available = true;
                    ESP_LOGD(TAG, "Stored %d log lines for node %d", (*pp_node)->log_count, node_id);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to allocate memory for log storage");
                }
            }

            // Release lock
            nodeio_unlock_logs();

            // Log preview (first 3 lines)
            int preview_count = ((*pp_node)->log_count < 3) ? (*pp_node)->log_count : 3;
            for (int i = 0; i < preview_count; i++)
            {
                ESP_LOGI(TAG, "  [%d]: %s", i, (*pp_node)->log_lines[i]);
            }
            if ((*pp_node)->log_count > preview_count)
            {
                ESP_LOGI(TAG, "  ... and %d more lines", (*pp_node)->log_count - preview_count);
            }
        }
    }

    cJSON_Delete(root);

    // Reset/extend active window on valid message from node
    modemanager_notify_activity_auto();

    // 3. Store/update node state, sensor values, etc.
    //    Example: update a struct or database with the latest info from this node

    // 4. Optionally, send a response or command back to this node
    //    websockserver_send(client_fd, response, strlen(response));

    // Note: transient parsed messages are stored in the per-node slots:
    // - periodic/node_data -> node_contexts[node_id].p_msg_data
    // - sporadic/node_event -> node_contexts[node_id].p_msg_event
    // Each slot is replaced (and previous entry freed) when a later message of
    // the same category arrives, and both slots are freed on disconnect.

    HEAP_TRACE_END(100); // Use higher threshold for message handling
}

static void nodeio_send_response(int client_fd, const char *response, size_t len)
{
    // Send a response back to the client
    websockserver_send(client_fd, response, len);
}

static void nodeio_broadcast(const char *message, size_t len)
{
    // Broadcast a message to all connected clients
    for (int i = 0; i < MAX_SESSIONS; ++i)
    {
        if (node_contexts[i].p_session->connected)
        {
            websockserver_send(node_contexts[i].p_session->client_fd, message, len);
        }
    }
}

static void nodeio_subscribe_to_node(int node_id, const subscribe_config_t *config)
{
    HEAP_TRACE_START("SUBSCRIBE");

    int client_fd = websockserver_session_find_fd(node_id);
    // Find the node context associated with this client_fd
    if (node_id < 0 || node_id >= MAX_NODES)
    {
        ESP_LOGW(TAG, "Subscribe: Invalid node_id for client_fd %d", client_fd);
        return;
    }

    if ((node_contexts[node_id].p_session) && (node_contexts[node_id].p_session->client_fd == client_fd))
    {
        // Check if subscription is to be updated
        if (node_contexts[node_id].subscription_update == true)
        {
            cJSON *root = cJSON_CreateObject();
            cJSON *payload = cJSON_CreateObject();
            if (config)
            {
                // update the node subscription parameters
                node_contexts[node_id].subscription.subscribe_mask = config->subscribe_mask;
                node_contexts[node_id].subscription.interval_ms = config->interval_ms;
                node_contexts[node_id].subscribed = true;

                ESP_LOGI(TAG, "Node %d subscribed: mask=0x%08X, interval=%u ms", node_id, config->subscribe_mask, config->interval_ms);
                // Header fields
                cJSON_AddNumberToObject(root, "magic", PROTOCOL_MAGIC);
                cJSON_AddStringToObject(root, "type", MSG_TYP_SUBSCRIBE);
                cJSON_AddNumberToObject(root, "node_id", node_id);
                cJSON_AddNumberToObject(root, "seq_num", node_local_seq[node_id]++);
                cJSON_AddNumberToObject(root, "timestamp", (uint32_t)time(NULL));
                // Payload
                cJSON_AddStringToObject(payload, "status", "subscribed");
                // Filter (example: sensors/services hardcoded for now)
                cJSON *filter = cJSON_CreateObject();
                cJSON *sensors = cJSON_CreateArray();
                // Dynamically add sensors based on subscribe_mask using sensors LUT
                const field_lookup_t *s_lut = nodeio_get_sensors_lut();
                size_t s_count = nodeio_get_sensors_lut_count();
                for (size_t s = 0; s < s_count; ++s)
                {
                    if (config->subscribe_mask & s_lut[s].cap)
                    {
                        cJSON_AddItemToArray(sensors, cJSON_CreateString(s_lut[s].name));
                    }
                }
                cJSON_AddItemToObject(filter, "sensors", sensors);
                // Dynamically add services based on subscribe_mask using service_table
                cJSON *services = cJSON_CreateArray();
                // Dynamically add services based on subscribe_mask using services LUT
                const field_lookup_t *sv_lut = nodeio_get_services_lut();
                size_t sv_count = nodeio_get_services_lut_count();
                for (size_t s = 0; s < sv_count; ++s)
                {
                    if (config->subscribe_mask & sv_lut[s].cap)
                    {
                        cJSON_AddItemToArray(services, cJSON_CreateString(sv_lut[s].name));
                    }
                }
                cJSON_AddItemToObject(filter, "services", services);
                cJSON_AddItemToObject(payload, "filter", filter);
                cJSON_AddNumberToObject(payload, "interval", config->interval_ms);
                cJSON_AddItemToObject(root, "payload", payload);
            }
            else
            {
                node_contexts[node_id].subscribed = false;
                node_contexts[node_id].subscription.subscribe_mask = 0;
                node_contexts[node_id].subscription.interval_ms = 0;
                ESP_LOGI(TAG, "Node %d unsubscribed (null config)", node_id);
                cJSON_AddNumberToObject(root, "magic", PROTOCOL_MAGIC);
                cJSON_AddStringToObject(root, "type", MSG_TYP_SUBSCRIBE);
                cJSON_AddNumberToObject(root, "node_id", node_id);
                cJSON_AddNumberToObject(root, "seq_num", node_local_seq[node_id]++);
                cJSON_AddNumberToObject(root, "timestamp", (uint32_t)time(NULL));
                cJSON_AddStringToObject(payload, "status", "unsubscribed");
                cJSON_AddItemToObject(root, "payload", payload);
            }
            char *msg = cJSON_PrintUnformatted(root);
            if (msg)
            {
                websockserver_send(client_fd, msg, strlen(msg));
                cJSON_free(msg);
            }

            node_contexts[node_id].subscription_update = false; // Reset the update flag
            cJSON_Delete(root);

            HEAP_TRACE_END_DEFAULT();
            return;
        }
        else
        {
            // ESP_LOGI(TAG, "No subscription update triggered for node id: %d", node_id);
        }
    }
    else
    {
        ESP_LOGW(TAG, "Subscribe: client_fd %d not found, node id: %d", client_fd, node_id);
    }

    HEAP_TRACE_END_DEFAULT();
    // check which parameter is causing the issue
    // if (!node_contexts[node_id].p_session)
    // {
    //     ESP_LOGW(TAG, "Subscribe: No session for node id %d", node_id);
    // }
    // else
    // {
    //     ESP_LOGW(TAG, "Subscribe: Session exists for node id %d, client_fd: %d, connected: %d", node_id, node_contexts[node_id].p_session->client_fd, node_contexts[node_id].p_session->connected);
    // }
}

void nodeio_process_subscription_updates(void)
{
    HEAP_TRACE_START("SUB_UPDATES");

    for (int i = 0; i < MAX_NODES; ++i)
    {
        // check if a node exists on this index
        if (node_contexts[i].p_node == NULL || node_contexts[i].p_session == NULL || !node_contexts[i].p_session->connected)
        {
            continue; // Skip if no node or session
        }
        // If there's a pending update flag, push the stored subscription to the node
        if (node_contexts[i].subscription_update)
        {
            // Clamp subscribe_mask to the node's capability so we don't request unsupported items
            capability_t mask = node_contexts[i].subscription.subscribe_mask & node_contexts[i].p_node->capability_mask;
            subscribe_config_t sub_config = {
                .subscribe_mask = mask,
                .interval_ms = node_contexts[i].subscription.interval_ms ? node_contexts[i].subscription.interval_ms : 5000};
            // If mask is zero, treat as unsubscribe (send null config)
            if (sub_config.subscribe_mask == 0)
            {
                nodeio_subscribe_to_node(node_contexts[i].p_node->node_id, NULL);
            }
            else
            {
                nodeio_subscribe_to_node(node_contexts[i].p_node->node_id, &sub_config);
            }
        }
    }

    HEAP_TRACE_END_DEFAULT();
}

// Public API: set subscription from Web UI and mark for update
esp_err_t nodeio_update_subscription(uint8_t node_id, capability_t subscribe_mask, uint32_t interval_ms)
{
    if (node_id >= MAX_NODES)
    {
        return ESP_ERR_INVALID_ARG;
    }
    node_context_t *ctx = &node_contexts[node_id];
    if (!ctx->p_node || !ctx->p_session || !ctx->p_session->connected)
    {
        return ESP_ERR_INVALID_STATE;
    }
    ctx->subscription.subscribe_mask = subscribe_mask;
    ctx->subscription.interval_ms = interval_ms;
    ctx->subscription_update = true; // trigger sending on next processing tick
    ctx->subscribed = (subscribe_mask != 0);
    ESP_LOGI(TAG, "UI subscription update for node %u: mask=0x%08X, interval=%u ms", (unsigned)node_id, (unsigned)subscribe_mask, (unsigned)interval_ms);
    return ESP_OK;
}

static void nodeio_unsubscribe_from_node(int client_fd)
{
    HEAP_TRACE_START("UNSUBSCRIBE");

    for (int i = 0; i < MAX_NODES; ++i)
    {
        if (node_contexts[i].p_session && node_contexts[i].p_session->client_fd == client_fd)
        {
            node_contexts[i].subscribed = false;
            ESP_LOGI(TAG, "Node %d unsubscribed", i);
            // Notify node of unsubscription (protocol-compliant)
            cJSON *root = cJSON_CreateObject();
            cJSON *payload = cJSON_CreateObject();
            cJSON_AddNumberToObject(root, "magic", PROTOCOL_MAGIC);
            cJSON_AddStringToObject(root, "type", MSG_TYP_SUBSCRIBE);
            cJSON_AddNumberToObject(root, "node_id", i);
            cJSON_AddNumberToObject(root, "seq_num", node_local_seq[i]++);
            cJSON_AddNumberToObject(root, "timestamp", (uint32_t)time(NULL));
            cJSON_AddStringToObject(payload, "status", "unsubscribed");
            cJSON_AddItemToObject(root, "payload", payload);
            char *msg = cJSON_PrintUnformatted(root);
            if (msg)
            {
                websockserver_send(client_fd, msg, strlen(msg));
                cJSON_free(msg);
            }
            cJSON_Delete(root);

            HEAP_TRACE_END_DEFAULT();
            return;
        }
    }
    ESP_LOGW(TAG, "Unsubscribe: client_fd %d not found", client_fd);

    HEAP_TRACE_END_DEFAULT();
}

static void nodeio_request_ota(int client_fd, const ota_request_t *ota)
{
    // Handle OTA request from node
    // Example: validate the request, log it, and prepare for OTA
}

static void nodeio_report_ota_status(int client_fd, const ota_status_t *status)
{
    // Handle OTA status report from node
    // Example: log the status, update UI, etc.
}

static void nodeio_send_connect_response(int client_fd, uint32_t seq_num)
{
    // Send a connection response message back to the node
    char resp_msg[128];
    // get node_id from session
    int node_id = websockserver_session_find_sessid(client_fd);
    ESP_LOGD(TAG, "Sending connect response to node id %d on client_fd %d", node_id, client_fd);
    /* Include the protocol magic in responses and use unsigned format specifiers for uint32_t values */
    int len = snprintf(resp_msg, sizeof(resp_msg), "{\"magic\":%u,\"type\":\"connect_response\",\"node_id\":%u,\"seq_num\":%u,\"timestamp\":%u,\"status\":\"accepted\"}",
                       (unsigned)PROTOCOL_MAGIC, (unsigned)node_id, (unsigned)seq_num, (unsigned)(uint32_t)time(NULL));
    if (len < 0 || len >= (int)sizeof(resp_msg))
    {
        ESP_LOGE(TAG, "Connect response message truncated or error occurred");
        return;
    }
    websockserver_send(client_fd, resp_msg, len);
}

static void nodeio_send_error(int client_fd, const char *error_msg)
{
    // Send an error message back to the node
    char err_msg[128];
    /* Include protocol envelope fields when sending errors */
    int node_id = websockserver_session_find_sessid(client_fd);
    unsigned u_node_id = (node_id >= 0) ? (unsigned)node_id : 0u;
    unsigned seq = (node_id >= 0) ? (unsigned)node_local_seq[node_id]++ : 0u;
    int len = snprintf(err_msg, sizeof(err_msg), "{\"magic\":%u,\"type\":\"error\",\"node_id\":%u,\"seq_num\":%u,\"timestamp\":%u,\"payload\":{\"message\":\"%s\"}}",
                       (unsigned)PROTOCOL_MAGIC, u_node_id, seq, (unsigned)(uint32_t)time(NULL), error_msg);
    if (len < 0 || len >= (int)sizeof(err_msg))
    {
        ESP_LOGE(TAG, "Error message formatting truncated or failed");
        return;
    }
    websockserver_send(client_fd, err_msg, len);
}

static void nodeio_process_diagnostic(int client_fd, const char *diag_info)
{
    // Process diagnostic information sent by the node
    // Example: log it, analyze it, etc.
}

static void nodeio_request_diagnostic(int client_fd)
{
    // Send a request to the node to provide diagnostic information
    /* Build protocol-compliant diagnostic request including magic and envelope */
    char diag_req[128];
    int node_id = websockserver_session_find_sessid(client_fd);
    unsigned u_node_id = (node_id >= 0) ? (unsigned)node_id : 0u;
    unsigned seq = (node_id >= 0) ? (unsigned)node_local_seq[node_id]++ : 0u;
    int len = snprintf(diag_req, sizeof(diag_req), "{\"magic\":%u,\"type\":\"%s\",\"node_id\":%u,\"seq_num\":%u,\"timestamp\":%u}",
                       (unsigned)PROTOCOL_MAGIC, MSG_TYP_DIAGNOSTIC_REQUEST, u_node_id, seq, (unsigned)(uint32_t)time(NULL));
    if (len < 0 || len >= (int)sizeof(diag_req))
    {
        ESP_LOGE(TAG, "diag request formatting truncated or failed");
        return;
    }
    websockserver_send(client_fd, diag_req, len);
}

static void nodeio_handle_disconnect(int client_fd, uint8_t node_id)
{
    HEAP_TRACE_START("DISCONNECT");

    // Check if it is a duplicate call to disconnect, can happen in case of direct disconnect request from node
    if (node_contexts[node_id].p_node == NULL && node_contexts[node_id].p_msg_data == NULL && node_contexts[node_id].p_msg_event == NULL)
    {
        // ESP_LOGW(TAG, "Node %d already disconnected, ignoring duplicate disconnect request", node_id);
        HEAP_TRACE_END_DEFAULT();
        return;
    }

    ESP_LOGD(TAG, "Node %d disconnect request", node_id);
    // Purge node context, free allocated memories (node and msg)
    node_params_t *p_node = node_contexts[node_id].p_node;
    if (p_node)
    {
        // Free stored logs if any
        if (p_node->log_lines)
        {
            for (int i = 0; i < p_node->log_count; i++)
            {
                free(p_node->log_lines[i]);
            }
            free(p_node->log_lines);
            p_node->log_lines = NULL;
        }

        // Free anomaly reason string if allocated
        if (p_node->last_anomaly_reason)
        {
            free(p_node->last_anomaly_reason);
            p_node->last_anomaly_reason = NULL;
        }

        free(p_node);
        node_contexts[node_id].p_node = NULL;
    }
    protocol_msg_t *p_msg = node_contexts[node_id].p_msg_data;
    if (p_msg)
    {
        free(p_msg);
        node_contexts[node_id].p_msg_data = NULL;
    }
    protocol_msg_t *p_evt = node_contexts[node_id].p_msg_event;
    if (p_evt)
    {
        free(p_evt);
        node_contexts[node_id].p_msg_event = NULL;
    }

    // Clear any stored logs for this node
    nodeio_clear_node_logs(node_id);

    // unsubscribe if subscribed
    nodeio_unsubscribe_from_node(client_fd);

    // Calculate and log node uptime
    if (node_contexts[node_id].node_uptime_start != 0)
    {
        node_contexts[node_id].node_uptime = (esp_timer_get_time() / 1000000) - node_contexts[node_id].node_uptime_start; // in seconds
        ESP_LOGI(TAG, "Node %d disconnected, uptime: %lld seconds", node_id, node_contexts[node_id].node_uptime);
        node_contexts[node_id].node_uptime_start = 0; // Reset start time
        node_contexts[node_id].last_uptime_log_ts = 0;
    }
    else
    {
        ESP_LOGI(TAG, "Node %d disconnected", node_id);
    }

    // Release all door sensors owned by this node
    for (int i = 0; i < NUM_DOOR_SENSORS; i++)
    {
        if (door_owner[i] == node_id)
        {
            ESP_LOGI(TAG, "Node %d released ownership of door sensor %d", node_id, i);
            door_owner[i] = -1;
        }
    }

    // Clear node_door_map entry for this node
    node_door_map[node_id] = -1;

    // Handle client disconnection
    websockserver_session_remove(client_fd);

    HEAP_TRACE_END_DEFAULT();
}

static node_params_t *nodeio_handle_connect(int client_fd, uint8_t node_id, cJSON *root)
{
    HEAP_TRACE_START("CONNECT");

    // Handle new client connection
    // local pointer to node_params_t*
    node_params_t **pp_node_to_connect = &node_contexts[node_id].p_node;
    if (*pp_node_to_connect == NULL)
    {
        // Node does not exist, create a new one
        node_params_t *newnode = calloc(1, sizeof(node_params_t));
        if (!newnode)
        {
            nodeio_handle_error(client_fd, "Failed to allocate memory");
            cJSON_Delete(root);
            return NULL;
        }
        *pp_node_to_connect = newnode;
    }
    else
    {
        // Node already exists, reject duplicate connect
        ESP_LOGD(TAG, "Node %d reconnected, current uptime: %lld seconds", node_id, node_contexts[node_id].node_uptime);
    }

    // nodeio_update_node_params_from_json is now handled outside of nodeio_handle_connect

    // until now p_session may or may not be assigned
    // websockserver updates it own session array, hence returning the pointer to the updated session array element
    node_contexts[node_id].p_session = websockserver_session_update(client_fd, node_id);
    ESP_LOGD(TAG, "Node %d connected with client FD: %d", node_id, client_fd);

    HEAP_TRACE_END_DEFAULT();

    return *pp_node_to_connect;
}

static void nodeio_handle_error(int client_fd, const char *error_msg)
{
    // Handle errors related to a specific client
    nodeio_send_error(client_fd, error_msg);
}

static void nodeio_handle_timeout(int client_fd)
{
    // Handle timeout events for a specific client
    nodeio_send_error(client_fd, "Timeout occurred");
}

static void nodeio_handle_heartbeat(int client_fd)
{
    // Handle heartbeat messages from the node
    /* Reply with a protocol envelope heartbeat ack */
    char heartbeat_ack[128];
    int node_id = websockserver_session_find_sessid(client_fd);
    unsigned u_node_id = (node_id >= 0) ? (unsigned)node_id : 0u;
    unsigned seq = (node_id >= 0) ? (unsigned)node_local_seq[node_id]++ : 0u;
    int len = snprintf(heartbeat_ack, sizeof(heartbeat_ack), "{\"magic\":%u,\"type\":\"heartbeat_ack\",\"node_id\":%u,\"seq_num\":%u,\"timestamp\":%u}",
                       (unsigned)PROTOCOL_MAGIC, u_node_id, seq, (unsigned)(uint32_t)time(NULL));
    if (len < 0 || len >= (int)sizeof(heartbeat_ack))
    {
        ESP_LOGE(TAG, "heartbeat ack formatting truncated or failed");
        return;
    }
    websockserver_send(client_fd, heartbeat_ack, len);
}

// Websocket server close callback
static void nodeio_on_close(int client_fd)
{
    HEAP_TRACE_START("ON_CLOSE");

    ESP_LOGD(TAG, "WebSocket client_fd %d on close callback", client_fd);
    // find node_id from client_fd
    int node_id = 0;
    // By design, node_id is the same as session_id
    node_id = websockserver_session_find_sessid(client_fd);
    if (node_id == -1)
    {
        ESP_LOGW(TAG, "Invalid node id in server close callback");
        return;
    }

    // Handle client disconnection
    nodeio_handle_disconnect(client_fd, (uint8_t)node_id);

    HEAP_TRACE_END_DEFAULT();
}

// Nodeio WebSocket server receive callback
static void nodeio_on_message(int client_fd, const char *data, size_t len)
{
    HEAP_TRACE_START("ON_MSG");

    // Handle incoming WebSocket messages
    nodeio_handle_message(client_fd, data, len);
    // TODO: Add any additional processing if needed

    HEAP_TRACE_END_DEFAULT();
}

// Monitor total number of connected nodes
void nodeio_monitor_nodeslist(void)
{
    HEAP_TRACE_START("MONITOR");

    static uint8_t prev_count = 0;
    uint8_t count = 0;
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (node_contexts[i].p_node != NULL && node_contexts[i].p_node->current_state == NODEIO_STATE_CONNECTED && node_contexts[i].p_session->connected)
        {
            count++;
        }
    }
    if (count != prev_count)
    {
        ESP_LOGI(TAG, "Total connected nodes: %d", count);
        prev_count = count;
    }

    // If there is any payload on any connected nodes, print it
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (node_contexts[i].p_node != NULL && node_contexts[i].p_node->current_state == NODEIO_STATE_CONNECTED && node_contexts[i].p_session->connected)
        {
            // Cache periodic and event message slots locally and iterate through payload only if present
            protocol_msg_t *pmsg = node_contexts[i].p_msg_data;  // periodic/data message
            protocol_msg_t *pevt = node_contexts[i].p_msg_event; // sporadic/event message
            if (pmsg)
            {
                for (int j = 0; j < pmsg->payload.payload_count; j++)
                {
                    // Print all periodic sensor values in a concise, tabular way using the sensors LUT
                    {
                        const field_lookup_t *s_lut = nodeio_get_sensors_lut();
                        size_t s_count = nodeio_get_sensors_lut_count();
                        for (size_t s = 0; s < s_count; ++s)
                        {
                            float fv = 0.0f;
                            if (nodeio_get_sensor_lut_field(pmsg, j, s_lut[s].name, 0, &fv, sizeof(fv)) == NIO_OK)
                            {
                                // ESP_LOGI(TAG, "Node %d sensor payload: %s: %.2f", i, s_lut[s].name, fv);
                            }
                        }
                    }

                    // (sporadic doorsense logging moved out of the per-payload loop)

                    // Print each service diag or ota value if present (serialize via service helper)
                    char serbuf[128];
                    if (nodeio_serialize_service_lut(pmsg, j, "diagnostics", serbuf, sizeof(serbuf)) == NIO_S_OK)
                    {
                        // ESP_LOGI(TAG, "Node %d service payload: Diag: %s", i, serbuf);
                    }
                    if (nodeio_serialize_service_lut(pmsg, j, "ota_status", serbuf, sizeof(serbuf)) == NIO_S_OK)
                    {
                        // ESP_LOGI(TAG, "Node %d service payload: OTA: %s", i, serbuf);
                    }
                }
                // After iterating periodic payloads, also print sporadic doorsense entries (if present) from the event slot
                if (pevt)
                {
                    const field_lookup_t *fld = nodeio_find_sensors_lut_field_by_name("doorsense");
                    if (fld && (pevt->payload.sporadic_data.current_cap_mask & fld->cap))
                    {
                        for (size_t di = 0; di < fld->elem_count; ++di)
                        {
                            uint8_t v = 0xFF;
                            if (nodeio_get_sensor_lut_field(pevt, 0, "doorsense", di, &v, sizeof(v)) == NIO_OK)
                            {
                                const char *sval = "UNKNOWN";
                                if (v == 1)
                                    sval = "OPEN";
                                else if (v == 0)
                                    sval = "CLOSED";
                                ESP_LOGI(TAG, "Node %d sporadic: doorsense_%u = %s", i, (unsigned)di, sval);
                            }
                        }
                    }
                    else
                    {
                        /* also accept legacy 'door_state' if present in sporadic current_cap_mask via aliasing */
                        const field_lookup_t *legacy = nodeio_find_sensors_lut_field_by_name("door_state");
                        if (legacy && (pevt->payload.sporadic_data.current_cap_mask & legacy->cap))
                        {
                            for (size_t di = 0; di < legacy->elem_count; ++di)
                            {
                                uint8_t v = 0xFF;
                                if (nodeio_get_sensor_lut_field(pevt, 0, "door_state", di, &v, sizeof(v)) == NIO_OK)
                                {
                                    const char *sval = "UNKNOWN";
                                    if (v == 1)
                                        sval = "OPEN";
                                    else if (v == 0)
                                        sval = "CLOSED";
                                    ESP_LOGI(TAG, "Node %d sporadic: door_state_%u = %s", i, (unsigned)di, sval);
                                }
                            }
                        }
                    }
                }
                // Log if node is online
                // ESP_LOGI(TAG, "Node %d is online", i);
                // Update and log node uptime
                if (node_contexts[i].node_uptime_start != 0)
                {
                    node_contexts[i].node_uptime = (esp_timer_get_time() / 1000000) - node_contexts[i].node_uptime_start; // in seconds
                    int64_t uptime = node_contexts[i].node_uptime;
                    int dd, hh, mm, ss;
                    dd = (int)(uptime / 86400);
                    hh = (int)((uptime / 3600) % 24);
                    mm = (int)((uptime / 60) % 60);
                    ss = (int)(uptime % 60);
                    char uptime_str[32];
                    if (dd > 0)
                        snprintf(uptime_str, sizeof(uptime_str), "%d:%02d:%02d:%02d", dd, hh, mm, ss);
                    else if (hh > 0)
                        snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d:%02d", hh, mm, ss);
                    else if (mm > 0)
                        snprintf(uptime_str, sizeof(uptime_str), "%02d:%02d", mm, ss);
                    else
                        snprintf(uptime_str, sizeof(uptime_str), "%02d", ss);

                    /* Throttle uptime logging to at most once per 60 seconds per node */
                    int64_t now_s = esp_timer_get_time() / 1000000;
                    if (node_contexts[i].last_uptime_log_ts == 0 || (now_s - node_contexts[i].last_uptime_log_ts) >= 60)
                    {
                        ESP_LOGI(TAG, "Node %d uptime: %s", i, uptime_str);
                        node_contexts[i].last_uptime_log_ts = now_s;
                    }
                }
                else
                {
                    ESP_LOGD(TAG, "Node %d uptime: not started", i);
                }
            }
        }

        // ESP_LOGI(TAG, "-----------------------------------");

        HEAP_TRACE_END_DEFAULT();
    }
}

size_t nodeio_publish_nodeslist(char *json, size_t json_size)
{
    HEAP_TRACE_START("PUBLISH");

    int offset = 0;
    int node_count = 0;

    // ESP_LOGD(TAG, "nodeio_publish_nodeslist called");

    /* Start JSON array. Use remaining-size guarded snprintf calls below. */
    if (json_size > 0)
        offset += snprintf(json + offset, json_size - offset, "[");
    else
        offset = 0;

    int first_node = 1;

    for (int i = 0; i < MAX_NODES; i++)
    {
        node_context_t *ctx = &node_contexts[i];
        /* Snapshot the periodic and event message pointers to avoid races while formatting JSON */
        protocol_msg_t *pmsg = ctx->p_msg_data;  /* periodic/data message */
        protocol_msg_t *pevt = ctx->p_msg_event; /* sporadic/event message */

        // ESP_LOGD(TAG, "Node context %d: p_node=%p, p_session=%p, p_msg=%p",
        //          i, (void *)ctx->p_node, (void *)ctx->p_session, (void *)ctx->p_msg);

        if (ctx->p_node && ctx->p_session && ctx->p_session->connected &&
            ctx->p_node->current_state == NODEIO_STATE_CONNECTED)
        {
            node_count++;
            if (!first_node)
            {
                if (json_append(json, json_size, &offset, ",") < 0)
                    break; // no space left or truncation
            }
            first_node = 0;

            int64_t uptime = ctx->node_uptime;

            // Map backend fields to frontend expectations
            const char *status = "Online";
            int uptime_s = (int)uptime;
            // placeholders reserved for extended summaries (not used in JSON path)

            // include capability mask and current subscription state for UI configuration
            {
                if (json_append(json, json_size, &offset, "{\"id\":%d,\"status\":\"%s\",\"uptime_s\":%d,\"cap_mask\":%u,\"sub\":{\"mask\":%u,\"interval_ms\":%u},",
                                ctx->p_node->node_id, status, uptime_s,
                                (unsigned)ctx->p_node->capability_mask,
                                (unsigned)ctx->subscription.subscribe_mask,
                                (unsigned)ctx->subscription.interval_ms) < 0)
                    break;

                // Add lifecycle tracking info if available
                if (ctx->p_node->last_wake_source[0] != '\0')
                {
                    if (json_append(json, json_size, &offset, "\"last_wake_source\":\"%s\",\"expected_uptime\":%d,\"prev_sleep\":%d,\"prev_uptime\":%d,",
                                    ctx->p_node->last_wake_source,
                                    ctx->p_node->expected_uptime_sec,
                                    ctx->p_node->prev_sleep_sec,
                                    ctx->p_node->prev_uptime_sec) < 0)
                        break;
                }

                // Add anomaly info if present
                if (ctx->p_node->anomaly_count > 0)
                {
                    if (json_append(json, json_size, &offset, "\"anomaly_count\":%u,",
                                    (unsigned)ctx->p_node->anomaly_count) < 0)
                        break;

                    if (ctx->p_node->last_anomaly_reason)
                    {
                        if (json_append(json, json_size, &offset, "\"last_anomaly_reason\":\"%s\",\"last_anomaly_refreshes\":%d,",
                                        ctx->p_node->last_anomaly_reason,
                                        ctx->p_node->last_anomaly_refreshes) < 0)
                            break;
                    }
                }
            }

            // Sensors: output all available from sensors LUT
            int first_sensor = 1;
            {
                const field_lookup_t *s_lut = nodeio_get_sensors_lut();
                size_t s_count = nodeio_get_sensors_lut_count();
                for (size_t s = 0; s < s_count; ++s)
                {
                    if (!first_sensor)
                    {
                        int rem = (int)(json_size - offset);
                        if (rem > 0)
                            offset += snprintf(json + offset, rem, ",");
                        else
                            break;
                    }
                    first_sensor = 0;
                    float fv = 0.0f;
                    if (pmsg && pmsg->payload.payload_count > 0 &&
                        nodeio_get_sensor_lut_field(pmsg, 0, s_lut[s].name, 0, &fv, sizeof(fv)) == NIO_OK)
                    {
                        if (json_append(json, json_size, &offset, "\"%s\":%.2f", s_lut[s].name, fv) < 0)
                            break;
                    }
                    else
                    {
                        if (json_append(json, json_size, &offset, "\"%s\":null", s_lut[s].name) < 0)
                            break;
                    }
                }
            }

            // Services: output all available from services LUT
            {
                /* If no sensors were emitted, the header already contains a
                   trailing comma; avoid emitting an extra comma in that case. */
                if (first_sensor == 1)
                {
                    if (json_append(json, json_size, &offset, "\"services\":{") < 0)
                        break;
                }
                else
                {
                    if (json_append(json, json_size, &offset, ",\"services\":{") < 0)
                        break;
                }
            }
            int first_service = 1;
            {
                const field_lookup_t *sv_lut = nodeio_get_services_lut();
                size_t sv_count = nodeio_get_services_lut_count();
                for (size_t s = 0; s < sv_count; ++s)
                {
                    char serbuf[256];
                    bool have_service_output = false;

                    if (pmsg && pmsg->payload.payload_count > 0 &&
                        nodeio_serialize_service_lut(pmsg, 0, sv_lut[s].name, serbuf, sizeof(serbuf)) == NIO_S_OK)
                    {
                        // We have serializable service data
                        have_service_output = true;
                    }

                    if (!have_service_output)
                    {
                        // Nothing to emit for this service; skip without writing commas
                        continue;
                    }

                    // Emit comma separator only if this is not the first emitted service entry
                    if (!first_service)
                    {
                        if (json_append(json, json_size, &offset, ",") < 0)
                            break;
                    }

                    // Mark that we've emitted at least one service
                    first_service = 0;

                    // If service is diagnostics, extract error_code quickly from serialized JSON
                    if (strcmp(sv_lut[s].name, "diagnostics") == 0)
                    {
                        const char *p = strstr(serbuf, "\"error_code\":");
                        int errval = 0;
                        if (p)
                            (void)sscanf(p, "\"error_code\":%d", &errval);
                        if (json_append(json, json_size, &offset, "\"%s\":%d", sv_lut[s].name, errval) < 0)
                            break;
                    }
                    else if (strcmp(sv_lut[s].name, "ota") == 0 || strcmp(sv_lut[s].name, "ota_status") == 0)
                    {
                        /* include service payload as raw JSON (avoid embedding JSON as a quoted string)
                           nodeio_serialize_service_lut() returns a JSON fragment; insert it directly. */
                        if (json_append(json, json_size, &offset, "\"%s\":%s", sv_lut[s].name, serbuf) < 0)
                            break;
                    }
                    else
                    {
                        if (json_append(json, json_size, &offset, "\"%s\":true", sv_lut[s].name) < 0)
                            break;
                    }
                }
            }

            /* Close the services object (even if empty) so JSON remains valid. */
            if (json_append(json, json_size, &offset, "}") < 0)
                break;

            /* Sporadic/event-driven sensors (e.g. doorsense) - include under "sporadic" */
            {
                if (json_append(json, json_size, &offset, ",\"sporadic\":{") < 0)
                    break;

                /* doorsense canonical field - emit only doors owned by this node */
                const field_lookup_t *ds_fld = nodeio_find_sensors_lut_field_by_name("doorsense");
                /* Prefer sporadic/event slot for doorsense values; fall back to periodic payload if present */
                protocol_msg_t *sporadic_source = pevt ? pevt : pmsg;
                if (sporadic_source && ds_fld && (sporadic_source->payload.sporadic_data.current_cap_mask & ds_fld->cap))
                {
                    /* For FIELD_TYPE_UINT8_ARRAY (doorsense) the getter expects a buffer
                       sized to the full element count; call it once and then emit each
                       element locally. This avoids passing a 1-byte buffer which the
                       getter rejects and which previously left values as UNKNOWN. */
                    {
                        size_t elem_cnt = ds_fld->elem_count;
                        uint8_t dsbuf[NUM_DOOR_SENSORS]; /* NUM_DOOR_SENSORS is compile-time constant in headers */
                        /* zero-init to unknown (0xFF) for safety */
                        for (size_t z = 0; z < elem_cnt; ++z)
                            dsbuf[z] = 0xFF;

                        /* Debug: log which source we use and current sporadic cap mask */
                        ESP_LOGD(TAG, "publish nodeslist: node %d doorsense source=%s ptr=%p sporadic_cap_mask=0x%08X",
                                 ctx->p_node->node_id, (pevt ? "event" : "periodic"), (void *)sporadic_source,
                                 (unsigned)sporadic_source->payload.sporadic_data.current_cap_mask);

                        if (nodeio_get_sensor_lut_field(sporadic_source, 0, "doorsense", 0, dsbuf, elem_cnt * sizeof(uint8_t)) == NIO_OK)
                        {
                            /* Only emit door indices owned by this node as individual keys (doorsense_N) */
                            int current_node_id = ctx->p_node->node_id;
                            int emitted_count = 0;

                            /* build a small readable string for logging */
                            char dbg[128];
                            int dbg_off = 0;

                            for (size_t di = 0; di < elem_cnt; ++di)
                            {
                                if (door_owner[di] != current_node_id)
                                    continue; /* Skip doors not owned by this node */

                                uint8_t v = dsbuf[di];
                                const char *sval = "UNKNOWN";
                                if (v == 1)
                                    sval = "OPEN";
                                else if (v == 0)
                                    sval = "CLOSED";

                                /* Emit as "doorsense_N":"STATE" */
                                if (json_append(json, json_size, &offset, "%s\"doorsense_%u\":\"%s\"",
                                                (emitted_count == 0) ? "" : ",", (unsigned)di, sval) < 0)
                                    break;

                                /* Build debug string */
                                if (dbg_off < (int)sizeof(dbg) - 16)
                                    dbg_off += snprintf(dbg + dbg_off, sizeof(dbg) - dbg_off,
                                                        "%sdoor_%u=%s", (dbg_off == 0) ? "" : ", ", (unsigned)di, sval);

                                emitted_count++;
                            }

                            dbg[sizeof(dbg) - 1] = '\0';
                            if (emitted_count > 0)
                                ESP_LOGD(TAG, "publish nodeslist: node %d doorsense (owned only): %s", ctx->p_node->node_id, dbg);
                            else
                                ESP_LOGD(TAG, "publish nodeslist: node %d doorsense (owned only): (none)", ctx->p_node->node_id);
                        }
                    }
                }
                else
                {
                    /* no doorsense capability or no events - don't emit anything */
                }

                /* close sporadic object */
                if (json_append(json, json_size, &offset, "}") < 0)
                    break;
            }

            /* Close node object */
            if (json_append(json, json_size, &offset, "}") < 0)
                break;
        }
    }

    if (json_size > 0)
        offset += snprintf(json + offset, (int)(json_size - offset), "]");

    // Ensure null termination within buffer
    if (offset >= (int)json_size)
        offset = (int)json_size - 1;
    if (json_size > 0)
        json[offset] = '\0';

    ESP_LOGD(TAG, "nodeio_publish_nodeslist: published %d nodes, JSON length: %d (buf %u)", node_count, offset, (unsigned)json_size);

    /* Sanity check: parse the generated JSON to detect formatting issues early */
    {
        cJSON *check = cJSON_Parse(json);
        if (!check)
        {
            /* Log a truncated snippet to avoid flooding logs */
            char snippet[512];
            int sn = (offset < (int)sizeof(snippet) - 1) ? offset : (int)sizeof(snippet) - 1;
            if (sn > 0)
            {
                memcpy(snippet, json, sn);
                snippet[sn] = '\0';
            }
            else
            {
                snippet[0] = '\0';
            }
            ESP_LOGE(TAG, "nodeio_publish_nodeslist: JSON parse failed; snippet: %s", snippet);
        }
        else
        {
            cJSON_Delete(check);
        }
    }

    HEAP_TRACE_END_DEFAULT();
    return offset;
}

/**
 * @brief Clear stored logs for a node (thread-safe)
 * Called on disconnect, reconnect, or timeout
 */
void nodeio_clear_node_logs(uint8_t node_id)
{
    if (node_id >= MAX_NODES)
        return;

    nodeio_lock_logs();

    node_params_t *p_node = node_contexts[node_id].p_node;
    if (p_node && p_node->log_lines)
    {
        for (int i = 0; i < p_node->log_count; i++)
        {
            if (p_node->log_lines[i])
                free(p_node->log_lines[i]);
        }
        free(p_node->log_lines);
        p_node->log_lines = NULL;
        p_node->log_count = 0;
        p_node->logs_available = false;
        p_node->log_timestamp = 0;
        ESP_LOGD(TAG, "Cleared logs for node %d", node_id);
    }

    nodeio_unlock_logs();
}

/**
 * @brief Lock logs mutex for thread-safe access
 */
void nodeio_lock_logs(void)
{
    if (g_logs_mutex)
        xSemaphoreTake(g_logs_mutex, portMAX_DELAY);
}

/**
 * @brief Unlock logs mutex
 */
void nodeio_unlock_logs(void)
{
    if (g_logs_mutex)
        xSemaphoreGive(g_logs_mutex);
}

esp_err_t nodeio_init(void)
{
    // Create mutex for log access protection
    g_logs_mutex = xSemaphoreCreateMutex();
    if (!g_logs_mutex)
    {
        ESP_LOGE(TAG, "Failed to create logs mutex");
        return ESP_ERR_NO_MEM;
    }

    // Set a less-verbose default log level to avoid noisy debug output in normal operation
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // Initialize node_door_map to -1 (no door assigned)
    for (int i = 0; i < MAX_NODES; i++)
    {
        node_door_map[i] = -1;
    }

    // Initialize WebSocket server
    ESP_LOGI(TAG, "NodeIO initialized");
    // websockserver_init() already called in webserver_init()
    // Set up the callbacks
    websockserver_set_receive_callback(nodeio_on_message);
    websockserver_set_close_callback(nodeio_on_close);
    ESP_LOGI(TAG, "NodeIO WebSocket server ready");
    return ESP_OK;
}

int nodeio_get_connected_node_count(void)
{
    int node_count = 0;
    for (int i = 0; i < MAX_NODES; i++)
    {
        node_context_t *ctx = &node_contexts[i];
        if (ctx->p_node && ctx->p_session && ctx->p_session->connected &&
            ctx->p_node->current_state == NODEIO_STATE_CONNECTED)
        {
            node_count++;
        }
    }
    return node_count;
}

node_params_t *nodeio_get_node_params(uint8_t node_id)
{
    if (node_id >= MAX_NODES)
    {
        return NULL;
    }
    return node_contexts[node_id].p_node;
}

esp_err_t nodeio_request_logs(uint8_t node_id, uint16_t max_lines)
{
    ESP_LOGI(TAG, "Requesting logs from node %d (max_lines: %d)", node_id, max_lines);

    // Validate node_id
    if (node_id >= MAX_NODES)
    {
        ESP_LOGE(TAG, "Invalid node_id %d (max: %d)", node_id, MAX_NODES - 1);
        return ESP_ERR_INVALID_ARG;
    }

    node_context_t *ctx = &node_contexts[node_id];

    // Validate node is connected and supports remote logging
    if (!ctx->p_node || !ctx->p_session || !ctx->p_session->connected)
    {
        ESP_LOGW(TAG, "Node %d not connected", node_id);
        return ESP_ERR_INVALID_STATE;
    }

    if (!(ctx->p_node->capability_mask & CAP_REMOTE_LOGGING))
    {
        ESP_LOGW(TAG, "Node %d does not support remote logging", node_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Build JSON message
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "magic", PROTOCOL_MAGIC);
    cJSON_AddStringToObject(root, "type", MSG_TYP_LOG_REQUEST);
    cJSON_AddNumberToObject(root, "node_id", node_id);
    cJSON_AddNumberToObject(root, "seq_num", node_local_seq[node_id]++);
    cJSON_AddNumberToObject(root, "timestamp", time(NULL));

    cJSON *payload = cJSON_CreateObject();
    if (!payload)
    {
        ESP_LOGE(TAG, "Failed to create payload object");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(payload, "max_lines", max_lines);
    cJSON_AddItemToObject(root, "payload", payload);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }

    // Send via WebSocket
    esp_err_t err = websockserver_send(ctx->p_session->client_fd, json_str, strlen(json_str)) ? ESP_OK : ESP_FAIL;
    free(json_str);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send log request to node %d: %s", node_id, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Log request sent to node %d", node_id);
    return ESP_OK;
}
