#include "websockserver.h"
#include "nodeio.h"
#include "esp_log.h"
#include "nodeioprotocol.h"
#include "cJSON.h"

// Maximum num of nodes shall be equal to the maximum number of sessions
#define MAX_NODES MAX_SESSIONS

static const char *TAG = "nodeio";

typedef struct
{
    node_params_t *p_node;
    protocol_msg_t *p_msg;
    wss_session_t *p_session;
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

static inline msg_type_t nodeio_type_str_to_enum(const char *type_str)
{
    ESP_LOGD(TAG, "Mapping type string: %s to enum", type_str);
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
                if (strcmp(sensor_name, "moisture") == 0)
                    current_mask |= CAP_MOISTURE;
                else if (strcmp(sensor_name, "temperature") == 0)
                    current_mask |= CAP_TEMP;
                else if (strcmp(sensor_name, "humidity") == 0)
                    current_mask |= CAP_HUMIDITY;
                else if (strcmp(sensor_name, "distance") == 0)
                    current_mask |= CAP_DISTANCE;
                else if (strcmp(sensor_name, "lightsense") == 0)
                    current_mask |= CAP_LIGHTSENSE;
                else if (strcmp(sensor_name, "led") == 0)
                    current_mask |= CAP_LED;
                else if (strcmp(sensor_name, "buzzer") == 0)
                    current_mask |= CAP_BUZZER;
                // Add more mappings as needed
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
                if (strcmp(service_name, "diagnostic") == 0)
                    current_mask |= CAP_DIAG;
                else if (strcmp(service_name, "ota") == 0)
                    current_mask |= CAP_OTA;
                // Add more mappings as needed
            }
        }
    }
    return current_mask;
}

// Helper to update node parameters from JSON
static inline node_params_t *nodeio_update_node_params_from_json(uint8_t node_id, cJSON *root)
{
    if (!root)
    {
        ESP_LOGE(TAG, "Invalid JSON root");
        return NULL;
    }
    // Only single pointer needed because we're just updating the structure fields
    node_params_t *node = node_contexts[node_id].p_node;
    if (!node)
    {
        ESP_LOGE(TAG, "Node pointer not assigned for this node id");
        return NULL;
    }

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
    return node;
}

// Helper function to parse payload from JSON
static inline esp_err_t nodeio_parse_message_payload(cJSON *root, capability_t node_cap_mask, protocol_msg_t *p_currentmsg)
{
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
                        p_currentmsg->payload.data[i].datafields.ota_status.status_code = status_code_item->valueint;
                        strncpy(p_currentmsg->payload.data[i].datafields.ota_status.message, message_item->valuestring, sizeof(p_currentmsg->payload.data[i].datafields.ota_status.message) - 1);
                        p_currentmsg->payload.data[i].datafields.ota_status.message[sizeof(p_currentmsg->payload.data[i].datafields.ota_status.message) - 1] = '\0';
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
                        p_currentmsg->payload.data[i].datafields.diagnostic.uptime_sec = uptime_item->valueint;
                        p_currentmsg->payload.data[i].datafields.diagnostic.free_heap = free_heap_item->valueint;
                        p_currentmsg->payload.data[i].datafields.diagnostic.rssi = rssi_item->valueint;
                        p_currentmsg->payload.data[i].datafields.diagnostic.error_code = error_code_item->valueint;
                        p_currentmsg->payload.data[i].current_cap_mask |= CAP_DIAG;
                    }
                }
            }
        }
        return ESP_OK;
    }
    else
    {
        ESP_LOGW(TAG, "No payload array found in message");
        return ESP_FAIL;
    }
}

static void nodeio_handle_message(int client_fd, const char *data, size_t len)
{
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
                // Successfully connected, send response
                nodeio_send_connect_response(client_fd, seq_num);
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
        }
    }
    cJSON_Delete(root);

    // 3. Store/update node state, sensor values, etc.
    //    Example: update a struct or database with the latest info from this node

    // 4. Optionally, send a response or command back to this node
    //    websockserver_send(client_fd, response, strlen(response));
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

static void nodeio_subscribe_to_node(int client_fd, const subscribe_config_t *config)
{
    // Handle subscription request from node
    // Store the subscription config for this client_fd
    // Example: update a struct or database with the subscription details
}

static void nodeio_unsubscribe_from_node(int client_fd)
{
    // Handle unsubscription request from node
    // Remove the subscription config for this client_fd
    // Example: update a struct or database to remove the subscription details
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
    char resp_msg[64];
    int len = snprintf(resp_msg, sizeof(resp_msg), "{\"type\":\"connect_response\",\"seq_num\":%lu,\"status\":\"accepted\"}", seq_num);
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

    // Handle client disconnection
    nodeio_unsubscribe_from_node(client_fd);
    websockserver_session_remove(client_fd);
    ESP_LOGI(TAG, "Node %d disconnected", node_id);
}

static node_params_t *nodeio_handle_connect(int client_fd, uint8_t node_id, cJSON *root)
{
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
        ESP_LOGD(TAG, "Node %d reconnected", node_id);
    }

    // nodeio_update_node_params_from_json is now handled outside of nodeio_handle_connect

    // until now p_session may or may not be assigned
    // websockserver updates it own session array, hence returning the pointer to the updated session array element
    node_contexts[node_id].p_session = websockserver_session_update(client_fd, node_id);
    ESP_LOGD(TAG, "Node %d connected with client FD: %d", node_id, client_fd);
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
    // find node_id from client_fd
    uint8_t node_id = 0;
    // By design, node_id is the same as session_id
    node_id = websockserver_session_find_sessid(client_fd);
    // Handle client disconnection
    nodeio_handle_disconnect(client_fd, node_id);
}

// Nodeio WebSocket server receive callback
static void nodeio_on_message(int client_fd, const char *data, size_t len)
{
    // Handle incoming WebSocket messages
    nodeio_handle_message(client_fd, data, len);
    // TODO: Add any additional processing if needed
}

// Monitor total number of connected nodes
void nodeio_monitor_nodeslist(void)
{
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
                    // ESP_LOGI(TAG, "Node %d service payload: Diag: %d", i, node_contexts[i].p_msg->payload.data[j].datafields.diagnostic.error_code);
                }
                if (node_contexts[i].p_msg->payload.data[j].current_cap_mask & CAP_OTA)
                {
                    // ESP_LOGI(TAG, "Node %d service payload: OTA: %s", i, node_contexts[i].p_msg->payload.data[j].datafields.ota_status.message);
                }
            }
            // Log if node is online
            ESP_LOGI(TAG, "Node %d is online", i);
        }
    }
    ESP_LOGI(TAG, "-----------------------------------");
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
