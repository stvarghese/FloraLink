#include "mqtthub.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "mqtthub";
static esp_mqtt_client_handle_t mqtt_client = NULL;

static void mqtthub_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

void mqtthub_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_HUB_CLIENT_ID,
        .credentials.username = MQTT_HUB_USERNAME,
        .credentials.authentication.password = MQTT_HUB_PASSWORD,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtthub_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "MQTT hub client started");
}

void mqtthub_subscribe_to_node_data(void)
{
    esp_mqtt_client_subscribe(mqtt_client, NODE_DATA_TOPIC, 1);
    ESP_LOGI(TAG, "Subscribed to node data topic: %s", NODE_DATA_TOPIC);
}

void mqtthub_subscribe_to_node_capabilities(void)
{
    esp_mqtt_client_subscribe(mqtt_client, NODE_CAPABILITIES_TOPIC, 1);
    ESP_LOGI(TAG, "Subscribed to node capabilities topic: %s", NODE_CAPABILITIES_TOPIC);
}

bool mqtthub_publish_config(const char *node_id, const char *config_json)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_CONFIG_TOPIC, node_id);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, config_json, 0, 1, 0);
    ESP_LOGI(TAG, "Published config to %s, msg_id=%d", topic, msg_id);
    return msg_id != -1;
}

bool mqtthub_publish_update(const char *node_id, const char *update_info_json)
{
    char topic[128];
    snprintf(topic, sizeof(topic), NODE_UPDATE_TOPIC, node_id);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, update_info_json, 0, 1, 0);
    ESP_LOGI(TAG, "Published update to %s, msg_id=%d", topic, msg_id);
    return msg_id != -1;
}

void mqtthub_on_node_data(const char *node_id, const char *data_json)
{
    ESP_LOGI(TAG, "Node %s data: %s", node_id, data_json);
    // TODO: Process node data
}

void mqtthub_on_node_capabilities(const char *node_id, const char *capabilities_json)
{
    ESP_LOGI(TAG, "Node %s capabilities: %s", node_id, capabilities_json);
    // TODO: Process node capabilities
}

static void mqtthub_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
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
        // Extract node_id from topic
        char node_id[64] = {0};
        const char *start = strstr(topic, "nodes/");
        if (start)
        {
            start += 6; // skip "nodes/"
            const char *slash = strchr(start, '/');
            if (slash && (slash - start) < (int)sizeof(node_id))
            {
                strncpy(node_id, start, slash - start);
                node_id[slash - start] = '\0';
                if (strstr(topic, "/data"))
                {
                    mqtthub_on_node_data(node_id, data);
                }
                else if (strstr(topic, "/capabilities"))
                {
                    mqtthub_on_node_capabilities(node_id, data);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}
