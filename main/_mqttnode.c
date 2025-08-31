#include "mqttnode.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mqttnode";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static char node_id_str[64];

static void mqttnode_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

void mqttnode_init(const char *node_id)
{
    strncpy(node_id_str, node_id, sizeof(node_id_str) - 1);
    node_id_str[sizeof(node_id_str) - 1] = '\0';
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_NODE_CLIENT_ID,
        .credentials.username = MQTT_NODE_USERNAME,
        .credentials.authentication.password = MQTT_NODE_PASSWORD,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqttnode_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT node client started");
}

bool mqttnode_publish_capabilities(const char *capabilities_json)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_CAPABILITIES_TOPIC, node_id_str);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, capabilities_json, 0, 1, 0);
    ESP_LOGI(TAG, "Published capabilities to %s, msg_id=%d", topic, msg_id);
    return msg_id != -1;
}

bool mqttnode_publish_data(const char *data_json)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_DATA_TOPIC, node_id_str);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, data_json, 0, 1, 0);
    ESP_LOGI(TAG, "Published data to %s, msg_id=%d", topic, msg_id);
    return msg_id != -1;
}

void mqttnode_subscribe_to_config(void)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_CONFIG_TOPIC, node_id_str);
    esp_mqtt_client_subscribe(mqtt_client, topic, 1);
    ESP_LOGI(TAG, "Subscribed to config topic: %s", topic);
}

void mqttnode_subscribe_to_update(void)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_UPDATE_TOPIC, node_id_str);
    esp_mqtt_client_subscribe(mqtt_client, topic, 1);
    ESP_LOGI(TAG, "Subscribed to update topic: %s", topic);
}

void mqttnode_on_config_received(const char *config_json)
{
    ESP_LOGI(TAG, "Config received: %s", config_json);
    // TODO: Apply configuration
}

void mqttnode_on_update_received(const char *update_info_json)
{
    ESP_LOGI(TAG, "Update received: %s", update_info_json);
    // TODO: Handle update (e.g., trigger OTA)
}

static void mqttnode_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_DATA:
    {
        char topic[event->topic_len + 1];
        char data[event->data_len + 1];
        memcpy(topic, event->topic, event->topic_len);
        topic[event->topic_len] = '\0';
        memcpy(data, event->data, event->data_len);
        data[event->data_len] = '\0';
        ESP_LOGI(TAG, "Received data on topic: %s", topic);
        if (strstr(topic, "/config"))
        {
            mqttnode_on_config_received(data);
        }
        else if (strstr(topic, "/update"))
        {
            mqttnode_on_update_received(data);
        }
        break;
    }
    default:
        break;
    }
}
