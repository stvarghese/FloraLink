#ifndef MQTTHUB_H
#define MQTTHUB_H

#include "mqtt_client.h"
#include <stdbool.h>

// MQTT broker configuration for hub
#define MQTT_BROKER_URI "mqtt://your_broker_address"
#define MQTT_HUB_CLIENT_ID "esp32_hub"
#define MQTT_HUB_USERNAME "your_username"
#define MQTT_HUB_PASSWORD "your_password"

// Topics for hub
#define NODE_CAPABILITIES_TOPIC "nodes/+/capabilities"
#define NODE_DATA_TOPIC "nodes/+/data"
#define NODE_CONFIG_TOPIC "nodes/%s/config"
#define NODE_UPDATE_TOPIC "nodes/%s/update"

// Hub MQTT initialization
void mqtthub_init(void);

// Subscribe to all node data and capabilities
void mqtthub_subscribe_to_node_data(void);
void mqtthub_subscribe_to_node_capabilities(void);

// Publish config and update to a node
bool mqtthub_publish_config(const char *node_id, const char *config_json);
bool mqtthub_publish_update(const char *node_id, const char *update_info_json);

// Callback for received node data
void mqtthub_on_node_data(const char *node_id, const char *data_json);

// Callback for received node capabilities
void mqtthub_on_node_capabilities(const char *node_id, const char *capabilities_json);

#endif // MQTTHUB_H
