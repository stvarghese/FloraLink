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

// Helper function to map type_str to msg_type_t
static inline msg_type_t nodeio_type_str_to_enum(const char *type_str)
{
    if (!type_str)
        return MSG_TYPE_UNKNOWN;
    switch (type_str[0])
    {
    case 'c':
        if (strcmp(type_str, "connect") == 0)
            return MSG_TYPE_CONNECT;
        break;
    case 'd':
        if (strcmp(type_str, "disconnect") == 0)
            return MSG_TYPE_DISCONNECT;
        else if (strcmp(type_str, "data") == 0)
            return MSG_TYPE_DATA;
        else if (strcmp(type_str, "diagnostic") == 0)
            return MSG_TYPE_DIAGNOSTIC;
        break;
    case 'h':
        if (strcmp(type_str, "heartbeat") == 0)
            return MSG_TYPE_HEARTBEAT;
        break;
    case 's':
        if (strcmp(type_str, "subscribe") == 0)
            return MSG_TYPE_SUBSCRIBE;
        break;
    case 'o':
        if (strcmp(type_str, "ota_request") == 0)
            return MSG_TYPE_OTA_REQUEST;
        else if (strcmp(type_str, "ota_status") == 0)
            return MSG_TYPE_OTA_STATUS;
        break;
    default:
        break;
    }
    return MSG_TYPE_UNKNOWN;
}

static void nodeio_handle_message(int client_fd, const char *data, size_t len)
{
    bool is_valid = false;
    // 1. Copy and null-terminate the data
    char msg[256];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';

    // 2. Parse the message as JSON and map to protocol types defined in nodeioprotocol.h
    cJSON *root = cJSON_Parse(msg);
    if (!root)
    {
        nodeio_handle_error(client_fd, "Invalid JSON");
        return;
    }

    // Extract magic (as per nodeioprotocol.h)
    cJSON *magic_item = cJSON_GetObjectItem(root, "magic");

    // Continue only if magic is valid
    if (!magic_item || !cJSON_IsNumber(magic_item) || magic_item->valueint != PROTOCOL_MAGIC)
    {
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

    // Get node context
    node_params_t *p_node = node_contextp[node_id].p_node;
    protocol_msg_t *p_currentmsg = node_contextp[node_id].p_msg;
    wss_session_t *p_session = node_contextp[node_id].p_session;

    p_currentmsg = calloc(1, sizeof(protocol_msg_t));
    if (!p_currentmsg)
    {
        nodeio_handle_error(client_fd, "Failed to allocate memory");
        cJSON_Delete(root);
        return;
    }

    node_contextp[node_id].p_msg = p_currentmsg;
    p_currentmsg->magic = magic_item->valueint;
    p_currentmsg->node_id = node_id;
    p_currentmsg->seq_num = seq_num;
    p_currentmsg->timestamp = timestamp;

    // Convert type_str to enum msg_type_t
    msg_type_t msg_type = nodeio_type_str_to_enum(type_str);
    p_currentmsg->type = msg_type;

    // Check if the message type is valid
    if (msg_type == MSG_TYPE_UNKNOWN)
    {
        nodeio_handle_error(client_fd, "Unknown message type");
        cJSON_Delete(root);
        return;
    }

    // For new, valid conn requests, check and create a new node context
    if (msg_type == MSG_TYPE_CONNECT)
    {
        if (p_node == NULL)
        {
            node_params_t *newnode = calloc(1, sizeof(node_params_t));
            if (!newnode)
            {
                nodeio_handle_error(client_fd, "Failed to allocate memory");
                cJSON_Delete(root);
                return;
            }
            node_contextp[node_id].p_node = newnode;
        }
        else
        {
            nodeio_handle_error(client_fd, "Node already connected");
        }

        // Parse further node parameters from the JSON, store after verification
        p_node->capability_mask = cJSON_GetObjectItem(root, "capability_mask") ? (capability_t)cJSON_GetObjectItem(root, "capability_mask")->valueint : 0;
        p_node->controller = cJSON_GetObjectItem(root, "controller") ? (controller_type_t)cJSON_GetObjectItem(root, "controller")->valueint : CONTROLLER_UNKNOWN;
        p_node->current_state = NODEIO_STATE_CONNECTED;
        cJSON *sw_version_item = cJSON_GetObjectItem(root, "sw_version");
        if (sw_version_item && cJSON_IsString(sw_version_item))
        {
            strncpy(newnode->sw_version, sw_version_item->valuestring, sizeof(newnode->sw_version) - 1);
            newnode->sw_version[sizeof(newnode->sw_version) - 1] = '\\0';
        }

        // Do session management for the new or updated node and store in node context
        node_contextp[node_id].p_session = websockserver_session_update(client_fd, node_id);
    }
    else if ((p_node == NULL) && (msg_type != MSG_TYPE_CONNECT))
    {
        nodeio_handle_error(client_fd, "Node not connected or unknown");
        cJSON_Delete(root);
        return;
    }

    // If msg_type DATA, process the data message after checking states
    if (msg_type == MSG_TYPE_DATA)
    {
        if ((p_session->client_fd == client_fd) && (p_node->current_state == NODEIO_STATE_CONNECTED))
        {
            p_currentmsg->payload = cJSON_GetObjectItem(root, "payload");
        }
    }

    // Map type string to protocol enum (example, adjust as needed)
    if (strcmp(type_str, "heartbeat") == 0)
    {
        nodeio_handle_heartbeat(client_fd);
    }
    else if (strcmp(type_str, "subscribe") == 0)
    {
        // Example: handle subscribe, parse config
        // cJSON *config = cJSON_GetObjectItem(root, "config");
        // TODO: parse config and call nodeio_subscribe_to_node
    }
    else if (strcmp(type_str, "ota_request") == 0)
    {
        // Example: handle OTA request
        // cJSON *ota = cJSON_GetObjectItem(root, "ota");
        // TODO: parse ota and call nodeio_request_ota
    }
    else if (strcmp(type_str, "diagnostic") == 0)
    {
        cJSON *diag_info = cJSON_GetObjectItem(root, "info");
        if (diag_info && cJSON_IsString(diag_info))
        {
            nodeio_process_diagnostic(client_fd, diag_info->valuestring);
        }
    }
    else
    {
        nodeio_send_error(client_fd, "Unknown message type");
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
        if (wss_activesessions[i].connected)
        {
            websockserver_send(wss_activesessions[i].client_fd, message, len);
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

static void nodeio_send_ack(int client_fd, uint32_t seq_num)
{
    // Send an acknowledgment message back to the node
    char ack_msg[64];
    int len = snprintf(ack_msg, sizeof(ack_msg), "{\"type\":\"ack\",\"seq_num\":%u}", seq_num);
    websockserver_send(client_fd, ack_msg, len);
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

static void nodeio_handle_disconnect(int client_fd)
{
    // Handle client disconnection
    nodeio_unsubscribe_from_node(client_fd);
    websockserver_session_remove(client_fd);
    ESP_LOGI(TAG, "Client %d disconnected", client_fd);
}

static void nodeio_handle_connect(int client_fd, const char *session_id)
{
    // Handle new client connection
    websockserver_session_add(client_fd, session_id);
    ESP_LOGI(TAG, "Client %d connected with session ID: %s", client_fd, session_id);
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
    // Handle client disconnection
    nodeio_handle_disconnect(client_fd);
}

// Nodeio WebSocket server receive callback
static void nodeio_on_message(int client_fd, const char *data, size_t len)
{
    // Handle incoming WebSocket messages
    nodeio_handle_message(client_fd, data, len);
    // TODO: Add any additional processing if needed
}

// Initialize nodeio(websocket) and wait for incoming connection requests
esp_err_t nodeio_init(void)
{
    // Initialize WebSocket server
    ESP_LOGI(TAG, "NodeIO initialized");
    // websockserver_init() already called in webserver_init()
    // Set up the receive callback
    websockserver_set_receive_callback(nodeio_on_message);
    ESP_LOGI(TAG, "NodeIO WebSocket server ready");
    return ESP_OK;
}
