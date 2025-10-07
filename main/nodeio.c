#include "websockserver.h"
#include "nodeio.h"
#include "modemanager.h"
#include "esp_log.h"
#include "nodeioprotocol.h"
#include "cJSON.h"
#include "commonutils.h"
#include <time.h>

// Maximum num of nodes shall be equal to the maximum number of sessions
#define MAX_NODES MAX_SESSIONS

static const char *TAG = "nodeio";

typedef struct
{
    node_params_t *p_node;
    protocol_msg_t *p_msg;
    wss_session_t *p_session;
    subscribe_config_t subscription; // Add this line
    bool subscribed;                 // Track if a subscription is active
    bool subscription_update;        // Track if subscription parameters were updated
    int64_t node_uptime_start;       // Timestamp when node connected
    int64_t node_uptime;             // Node uptime in seconds
} node_context_t;

static node_context_t node_contexts[MAX_NODES] = {0};

typedef struct
{
    const char *name;
    capability_t cap;
    size_t offset;
} sensor_lookup_t;

static const sensor_lookup_t sensor_table[] = {
    /* Sensor Name   Capability Mask   Offset within sensor_payload_t */
    {"temperature", CAP_TEMP, offsetof(sensor_payload_t, temp)},
    {"moisture", CAP_MOISTURE, offsetof(sensor_payload_t, moisture)},
    {"humidity", CAP_HUMIDITY, offsetof(sensor_payload_t, humidity)},
    {"distance", CAP_DISTANCE, offsetof(sensor_payload_t, distance)},
    {"light", CAP_LIGHTSENSE, offsetof(sensor_payload_t, light)},
    // Add more sensor types as needed
};

// Service lookup table for dynamic service filter
typedef struct
{
    const char *name;
    capability_t cap;
    size_t offset;
} service_lookup_t;

static const service_lookup_t service_table[] = {
    {"diagnostics", CAP_DIAG, offsetof(service_payload_t, diagnostics)},
    {"ota", CAP_OTA, offsetof(service_payload_t, ota_status)},
    // Add more services as needed
};

// Per node local sequence number
static uint32_t node_local_seq[MAX_NODES] = {0};

// Function declarations
static inline msg_type_t nodeio_type_str_to_enum(const char *type_str);
static inline esp_err_t nodeio_parse_message_payload(cJSON *root, capability_t node_cap_mask, protocol_msg_t *p_currentmsg);
static inline capability_t nodeio_build_node_capmask(cJSON *sensors_array);
static void nodeio_handle_message(int client_fd, const char *data, size_t len);
static void nodeio_send_response(int client_fd, const char *response, size_t len);
static void nodeio_broadcast(const char *message, size_t len);
static void nodeio_subscribe_to_node(int client_fd, const subscribe_config_t *config);
static void nodeio_unsubscribe_from_node(int client_fd);
static void nodeio_request_ota(int client_fd, const ota_request_t *ota);
static void nodeio_report_ota_status(int client_fd, const ota_status_t *status);
static void nodeio_send_connect_response(int client_fd, uint32_t seq_num);
static void nodeio_send_error(int client_fd, const char *error_msg);
static void nodeio_process_diagnostic(int client_fd, const char *diag_info);
static void nodeio_request_diagnostic(int client_fd);
static void nodeio_handle_disconnect(int client_fd, uint8_t node_id);
static node_params_t *nodeio_handle_connect(int client_fd, uint8_t node_id, cJSON *root);
static void nodeio_handle_error(int client_fd, const char *error_msg);
static void nodeio_handle_timeout(int client_fd);
static void nodeio_handle_heartbeat(int client_fd);
static void nodeio_on_close(int client_fd);
static void nodeio_on_message(int client_fd, const char *data, size_t len);

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
    {MSG_TYP_SUBSCRIBE, MSG_SUBSCRIBE},
    {MSG_TYP_POLL_DATA, MSG_POLL_DATA},
    {MSG_TYP_OTA_REQUEST, MSG_OTA_REQUEST},
    {MSG_TYP_OTA_STATUS, MSG_OTA_STATUS},
    {MSG_TYP_DIAGNOSTIC, MSG_DIAGNOSTIC},
    {MSG_TYP_DIAGNOSTIC_REQUEST, MSG_DIAGNOSTIC_REQUEST},
    {MSG_TYP_ACK, MSG_ACK},
    {MSG_TYP_HEARTBEAT, MSG_HEARTBEAT},
    {MSG_TYP_PING, MSG_PING},
    {MSG_TYP_PONG, MSG_PONG},
    {MSG_TYP_DISCONNECT_REQUEST, MSG_DISCONNECT_REQUEST},
    {MSG_TYP_ERROR, MSG_ERROR},
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
                ESP_LOGD(TAG, "Pinging node id %d on client_fd %d", node_contexts[i].p_node->node_id, client_fd);
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
static inline capability_t nodeio_build_node_capmask_sensors(cJSON *sensors_array, capability_t current_mask)
{
    if (sensors_array && cJSON_IsArray(sensors_array))
    {
        int sensor_count = cJSON_GetArraySize(sensors_array);
        for (int i = 0; i < sensor_count; ++i)
        {
            cJSON *sensor_item = cJSON_GetArrayItem(sensors_array, i);
            if (sensor_item && cJSON_IsString(sensor_item))
            {
                const char *sensor_name = sensor_item->valuestring;
                for (size_t s = 0; s < sizeof(sensor_table) / sizeof(sensor_table[0]); ++s)
                {
                    if (strcmp(sensor_name, sensor_table[s].name) == 0)
                    {
                        current_mask |= sensor_table[s].cap;
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
                for (size_t s = 0; s < sizeof(service_table) / sizeof(service_table[0]); ++s)
                {
                    if (strcmp(service_name, service_table[s].name) == 0)
                    {
                        current_mask |= service_table[s].cap;
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

    cJSON *payload_array = cJSON_GetObjectItem(root, "payload");
    if (payload_array && cJSON_IsArray(payload_array))
    {
        int count = cJSON_GetArraySize(payload_array);
        p_currentmsg->payload.payload_count = (count > PROTOCOL_MAX_PAYLOAD_COUNT) ? PROTOCOL_MAX_PAYLOAD_COUNT : count;
        for (int i = 0; i < p_currentmsg->payload.payload_count; ++i)
        {
            cJSON *item = cJSON_GetArrayItem(payload_array, i);
            cJSON *type_item = cJSON_GetObjectItem(item, "type");
            if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "sensor") == 0)
            {
                cJSON *sensor_obj = cJSON_GetObjectItem(item, "sensor");
                if (sensor_obj && cJSON_IsObject(sensor_obj))
                {
                    cJSON *field = sensor_obj->child;
                    while (field)
                    {
                        const char *key = field->string;
                        bool valid = false;
                        for (size_t s = 0; s < sizeof(sensor_table) / sizeof(sensor_table[0]); ++s)
                        {
                            // Compare the JSON key to the sensor name in the lookup table
                            if (strcmp(key, sensor_table[s].name) == 0)
                            {
                                // If the node's capability mask includes this sensor type
                                if (node_cap_mask & sensor_table[s].cap)
                                {
                                    /*
                                     * Pointer arithmetic explanation:
                                     * Each sensor value (e.g., temp, humidity) is a field in the sensor_payload_t struct.
                                     * sensor_table[s].offset gives the byte offset of the field within sensor_payload_t.
                                     * (uint8_t *)&p_currentmsg->payload.data[i].datafields.sensor casts the struct pointer to a byte pointer,
                                     * so we can add the offset in bytes to reach the correct field.
                                     * (float *) casts the result to a float pointer, so we can assign the value directly.
                                     * This allows us to generically set any sensor field using the lookup table.
                                     */
                                    float *pval = (float *)((uint8_t *)&p_currentmsg->payload.data[i].datafields.sensor + sensor_table[s].offset);
                                    *pval = (float)field->valuedouble;
                                    p_currentmsg->payload.data[i].current_cap_mask |= sensor_table[s].cap;
                                    valid = true;
                                }
                                else
                                {
                                    ESP_LOGD(TAG, "Sensor '%s' capability mask (%08X) not met", key, sensor_table[s].cap);
                                }
                                break;
                            }
                        }
                        if (!valid)
                        {
                            ESP_LOGW(TAG, "Sensor '%s' not in node capability mask (%08X), ignoring", key, node_cap_mask);
                        }
                        field = field->next;
                    }
                }
            }
            // Handle other types (ota_status, diagnostic) as before...
            else if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "ota_status") == 0)
            {
                cJSON *ota_obj = cJSON_GetObjectItem(item, "ota_status");
                if (ota_obj && cJSON_IsObject(ota_obj))
                {
                    cJSON *status_code_item = cJSON_GetObjectItem(ota_obj, "status_code");
                    cJSON *message_item = cJSON_GetObjectItem(ota_obj, "message");
                    if (status_code_item && cJSON_IsNumber(status_code_item) &&
                        message_item && cJSON_IsString(message_item))
                    {
                        p_currentmsg->payload.data[i].datafields.service.ota_status.status_code = status_code_item->valueint;
                        strncpy(p_currentmsg->payload.data[i].datafields.service.ota_status.message, message_item->valuestring, sizeof(p_currentmsg->payload.data[i].datafields.service.ota_status.message) - 1);
                        p_currentmsg->payload.data[i].datafields.service.ota_status.message[sizeof(p_currentmsg->payload.data[i].datafields.service.ota_status.message) - 1] = '\0';
                        p_currentmsg->payload.data[i].current_cap_mask |= CAP_OTA;
                    }
                }
            }
            else if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "diagnostics") == 0)
            {
                cJSON *diag_obj = cJSON_GetObjectItem(item, "diagnostics");
                if (diag_obj && cJSON_IsObject(diag_obj))
                {
                    cJSON *uptime_item = cJSON_GetObjectItem(diag_obj, "uptime_sec");
                    cJSON *free_heap_item = cJSON_GetObjectItem(diag_obj, "free_heap");
                    cJSON *rssi_item = cJSON_GetObjectItem(diag_obj, "rssi");
                    cJSON *error_code_item = cJSON_GetObjectItem(diag_obj, "error_code");
                    // Optionally handle 'info' string if needed in the future
                    if (uptime_item && cJSON_IsNumber(uptime_item) &&
                        free_heap_item && cJSON_IsNumber(free_heap_item) &&
                        rssi_item && cJSON_IsNumber(rssi_item) &&
                        error_code_item && cJSON_IsNumber(error_code_item))
                    {
                        p_currentmsg->payload.data[i].datafields.service.diagnostics.uptime_sec = uptime_item->valueint;
                        p_currentmsg->payload.data[i].datafields.service.diagnostics.free_heap = free_heap_item->valueint;
                        p_currentmsg->payload.data[i].datafields.service.diagnostics.rssi = rssi_item->valueint;
                        p_currentmsg->payload.data[i].datafields.service.diagnostics.error_code = error_code_item->valueint;
                        p_currentmsg->payload.data[i].current_cap_mask |= CAP_DIAG;
                    }
                }
            }
        }

        HEAP_TRACE_END_DEFAULT();
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "No payload array found in message");

        HEAP_TRACE_END_DEFAULT();
        return ESP_FAIL;
    }
}

static void nodeio_handle_message(int client_fd, const char *data, size_t len)
{
    HEAP_TRACE_START("NODEIO");
    bool is_valid = false;

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
    if (node_contexts[node_id].p_msg != NULL)
    {
        free(node_contexts[node_id].p_msg);
        node_contexts[node_id].p_msg = NULL;
    }

    // Allocate a new protocol_msg_t for this message
    protocol_msg_t *p_currentmsg = calloc(1, sizeof(protocol_msg_t));
    if (!p_currentmsg)
    {
        nodeio_handle_error(client_fd, "Failed to allocate memory");
        cJSON_Delete(root);
        return;
    }

    node_contexts[node_id].p_msg = p_currentmsg;
    p_currentmsg->magic = magic_item->valueint;
    p_currentmsg->node_id = node_id;
    p_currentmsg->seq_num = seq_num;
    p_currentmsg->timestamp = timestamp;

    // Convert type_str to enum msg_type_t
    msg_type_t msg_type = nodeio_type_str_to_enum(type_str);
    p_currentmsg->type = msg_type;

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
                // Record the node uptime start time
                node_contexts[node_id].node_uptime_start = esp_timer_get_time() / 1000000; // in seconds
                // Reset node uptime
                node_contexts[node_id].node_uptime = 0;
                // Successfully connected, send response
                nodeio_send_connect_response(client_fd, seq_num);
                // Trigger initial subscription update
                node_contexts[node_id].subscription_update = true;
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
            // Parse message payload
            if (nodeio_parse_message_payload(root, (*pp_node)->capability_mask, p_currentmsg) != ESP_OK)
            {
                nodeio_handle_error(client_fd, "Failed to parse message payload");
                cJSON_Delete(root);
                return;
            }
            // Reset/extend active window on valid node data (auto sleep version)
            modemanager_notify_activity_auto();
        }
    }
#include "modemanager.h"
    cJSON_Delete(root);

    // 3. Store/update node state, sensor values, etc.
    //    Example: update a struct or database with the latest info from this node

    // 4. Optionally, send a response or command back to this node
    //    websockserver_send(client_fd, response, strlen(response));

    // Note: p_currentmsg is now stored in node_contexts[node_id].p_msg and will be:
    // - Replaced when the next message from this node arrives
    // - Freed when the node disconnects in nodeio_handle_disconnect()

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
                // Dynamically add sensors based on subscribe_mask using sensor_table
                for (size_t s = 0; s < sizeof(sensor_table) / sizeof(sensor_table[0]); ++s)
                {
                    if (config->subscribe_mask & sensor_table[s].cap)
                    {
                        cJSON_AddItemToArray(sensors, cJSON_CreateString(sensor_table[s].name));
                    }
                }
                cJSON_AddItemToObject(filter, "sensors", sensors);
                // Dynamically add services based on subscribe_mask using service_table
                cJSON *services = cJSON_CreateArray();
                for (size_t s = 0; s < sizeof(service_table) / sizeof(service_table[0]); ++s)
                {
                    if (config->subscribe_mask & service_table[s].cap)
                    {
                        cJSON_AddItemToArray(services, cJSON_CreateString(service_table[s].name));
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

        // TODO: get sub config from web UI or other source
        subscribe_config_t sub_config = {
            .subscribe_mask = node_contexts[i].p_node->capability_mask, // Example: subscribe to all available sensors/services from node
            .interval_ms = 5000                                         // Example: 5 seconds interval
        };
        // Trigger subscription update
        // ESP_LOGI(TAG, "Processing subscription update for node id: %d", i);
        nodeio_subscribe_to_node(node_contexts[i].p_node->node_id, &sub_config);
    }

    HEAP_TRACE_END_DEFAULT();
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
    int len = snprintf(resp_msg, sizeof(resp_msg), "{\"type\":\"connect_response\",\"node_id\":%d,\"seq_num\":%lu,\"timestamp\":%lu,\"status\":\"accepted\"}", node_id, seq_num, (uint32_t)time(NULL));
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
    int len = snprintf(err_msg, sizeof(err_msg), "{\"type\":\"error\",\"message\":\"%s\"}", error_msg);
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
    const char *diag_req = "{\"type\":\"diag_request\"}";
    websockserver_send(client_fd, diag_req, strlen(diag_req));
}

static void nodeio_handle_disconnect(int client_fd, uint8_t node_id)
{
    HEAP_TRACE_START("DISCONNECT");

    // Check if it is a duplicate call to disconnect, can happen in case of direct disconnect request from node
    if (node_contexts[node_id].p_node == NULL && node_contexts[node_id].p_msg == NULL)
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
        free(p_node);
        node_contexts[node_id].p_node = NULL;
    }
    protocol_msg_t *p_msg = node_contexts[node_id].p_msg;
    if (p_msg)
    {
        free(p_msg);
        node_contexts[node_id].p_msg = NULL;
    }

    // Calculate and log node uptime
    if (node_contexts[node_id].node_uptime_start != 0)
    {
        node_contexts[node_id].node_uptime = (esp_timer_get_time() / 1000000) - node_contexts[node_id].node_uptime_start; // in seconds
        ESP_LOGI(TAG, "Node %d disconnected, uptime: %lld seconds", node_id, node_contexts[node_id].node_uptime);
        node_contexts[node_id].node_uptime_start = 0; // Reset start time
    }

    // Handle client disconnection
    nodeio_unsubscribe_from_node(client_fd);
    websockserver_session_remove(client_fd);
    ESP_LOGI(TAG, "Node %d disconnected", node_id);

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
    const char *heartbeat_ack = "{\"type\":\"heartbeat_ack\"}";
    websockserver_send(client_fd, heartbeat_ack, strlen(heartbeat_ack));
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
            // Iterate through payload
            for (int j = 0; j < node_contexts[i].p_msg->payload.payload_count; j++)
            {
                // Print all sensor values in a concise, tabular way using the lookup table
                for (size_t s = 0; s < sizeof(sensor_table) / sizeof(sensor_table[0]); ++s)
                {
                    if (node_contexts[i].p_msg->payload.data[j].current_cap_mask & sensor_table[s].cap)
                    {
                        float *pval = (float *)((uint8_t *)&node_contexts[i].p_msg->payload.data[j].datafields.sensor + sensor_table[s].offset);
                        // ESP_LOGI(TAG, "Node %d sensor payload: %s: %.2f", i, sensor_table[s].name, *pval);
                    }
                }
                // Print each service diag or ota value if present
                if (node_contexts[i].p_msg->payload.data[j].current_cap_mask & CAP_DIAG)
                {
                    // ESP_LOGI(TAG, "Node %d service payload: Diag: %d", i, node_contexts[i].p_msg->payload.data[j].datafields.service.diagnostics.error_code);
                }
                if (node_contexts[i].p_msg->payload.data[j].current_cap_mask & CAP_OTA)
                {
                    // ESP_LOGI(TAG, "Node %d service payload: OTA: %s", i, node_contexts[i].p_msg->payload.data[j].datafields.service.ota_status.message);
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
                ESP_LOGI(TAG, "Node %d uptime: %s", i, uptime_str);
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

size_t nodeio_publish_nodeslist(char *json, size_t json_size)
{
    HEAP_TRACE_START("PUBLISH");

    int offset = 0;
    int node_count = 0;

    // ESP_LOGD(TAG, "nodeio_publish_nodeslist called");

    offset += snprintf(json + offset, json_size - offset, "[");

    int first_node = 1;

    for (int i = 0; i < MAX_NODES; i++)
    {
        node_context_t *ctx = &node_contexts[i];

        // ESP_LOGD(TAG, "Node context %d: p_node=%p, p_session=%p, p_msg=%p",
        //          i, (void *)ctx->p_node, (void *)ctx->p_session, (void *)ctx->p_msg);

        if (ctx->p_node && ctx->p_session && ctx->p_session->connected &&
            ctx->p_node->current_state == NODEIO_STATE_CONNECTED)
        {
            node_count++;
            if (!first_node)
                offset += snprintf(json + offset, json_size - offset, ",");
            first_node = 0;

            int64_t uptime = ctx->node_uptime;

            // Map backend fields to frontend expectations
            const char *status = "Online";
            int uptime_s = (int)uptime;
            float temp = 0, humid = 0, batt = 0, moisture = 0;
            int has_temp = 0, has_humid = 0, has_batt = 0, has_moisture = 0;

            offset += snprintf(json + offset, json_size - offset,
                               "{\"id\":%d,\"status\":\"%s\",\"uptime_s\":%d,",
                               ctx->p_node->node_id, status, uptime_s);

            // Sensors: output all available from sensor_table
            int first_sensor = 1;
            for (size_t s = 0; s < sizeof(sensor_table) / sizeof(sensor_table[0]); ++s)
            {
                if (!first_sensor)
                {
                    offset += snprintf(json + offset, json_size - offset, ",");
                }
                first_sensor = 0;
                if (ctx->p_msg && ctx->p_msg->payload.payload_count > 0 &&
                    (ctx->p_msg->payload.data[0].current_cap_mask & sensor_table[s].cap))
                {
                    float *pval = (float *)((uint8_t *)&ctx->p_msg->payload.data[0].datafields.sensor + sensor_table[s].offset);
                    offset += snprintf(json + offset, json_size - offset, "\"%s\":%.2f", sensor_table[s].name, *pval);
                }
                else
                {
                    offset += snprintf(json + offset, json_size - offset, "\"%s\":null", sensor_table[s].name);
                }
            }

            // Services: output all available from service_table
            offset += snprintf(json + offset, json_size - offset, ",\"services\":{");
            int first_service = 1;
            for (size_t s = 0; s < sizeof(service_table) / sizeof(service_table[0]); ++s)
            {
                if (ctx->p_msg && ctx->p_msg->payload.payload_count > 0 &&
                    (ctx->p_msg->payload.data[0].current_cap_mask & service_table[s].cap))
                {
                    if (!first_service)
                        offset += snprintf(json + offset, json_size - offset, ",");
                    first_service = 0;
                    // Output service value if available, else true
                    if (strcmp(service_table[s].name, "diagnostics") == 0)
                    {
                        int err = ctx->p_msg->payload.data[0].datafields.service.diagnostics.error_code;
                        offset += snprintf(json + offset, json_size - offset, "\"%s\":%d", service_table[s].name, err);
                    }
                    else if (strcmp(service_table[s].name, "ota") == 0)
                    {
                        const char *msg = ctx->p_msg->payload.data[0].datafields.service.ota_status.message;
                        offset += snprintf(json + offset, json_size - offset, "\"%s\":\"%s\"", service_table[s].name, msg ? msg : "");
                    }
                    else
                    {
                        offset += snprintf(json + offset, json_size - offset, "\"%s\":true", service_table[s].name);
                    }
                }
            }
            offset += snprintf(json + offset, json_size - offset, "}}");
        }
    }

    offset += snprintf(json + offset, json_size - offset, "]");

    // Ensure null termination within buffer
    if (offset >= json_size)
        offset = json_size - 1;
    json[offset] = '\0';

    // ESP_LOGI(TAG, "nodeio_publish_nodeslist: published %d nodes, JSON length: %d", node_count, offset);

    HEAP_TRACE_END_DEFAULT();
    return offset;
}

// Initialize nodeio(websocket) and wait for incoming connection requests
esp_err_t nodeio_init(void)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
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
