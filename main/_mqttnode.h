#ifndef MQTTNODE_H
#define MQTTNODE_H

#include "mqtt_client.h"
#include <stdbool.h>

// MQTT broker configuration for node
#define MQTT_BROKER_URI "mqtt://your_broker_address"
#define MQTT_NODE_CLIENT_ID "node_sensor"
#define MQTT_NODE_USERNAME "your_username"
#define MQTT_NODE_PASSWORD "your_password"

// Topics for node
#define NODE_CAPABILITIES_TOPIC "nodes/%s/capabilities"
#define NODE_DATA_TOPIC "nodes/%s/data"
#define NODE_CONFIG_TOPIC "nodes/%s/config"
#define NODE_UPDATE_TOPIC "nodes/%s/update"

// Node MQTT initialization
void mqttnode_init(const char *node_id);

// Publish node capabilities
bool mqttnode_publish_capabilities(const char *capabilities_json);

// Publish sensor data
bool mqttnode_publish_data(const char *data_json);

// Subscribe to config and update topics
void mqttnode_subscribe_to_config(void);
void mqttnode_subscribe_to_update(void);

// Callback for received config
void mqttnode_on_config_received(const char *config_json);

// Callback for received update
void mqttnode_on_update_received(const char *update_info_json);

#endif // MQTTNODE_H
