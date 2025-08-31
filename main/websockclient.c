#include "websockclient.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "websock";
static esp_websocket_client_handle_t client = NULL;
static void (*receive_callback)(const char *, size_t) = NULL;
s static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    if (event_id == WEBSOCKET_EVENT_DATA && receive_callback)
    {
        receive_callback((const char *)data->data_ptr, data->data_len);
    }
}

bool websock_init(const char *uri)
{
    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
    };
    client = esp_websocket_client_init(&websocket_cfg);
    if (!client)
        return false;
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    return esp_websocket_client_start(client) == ESP_OK;
}

bool websock_send(const char *data, size_t len)
{
    if (!client)
        return false;
    return esp_websocket_client_send_text(client, data, len, portMAX_DELAY) > 0;
}

void websock_set_receive_callback(void (*callback)(const char *data, size_t len))
{
    receive_callback = callback;
}

void websock_close(void)
{
    if (client)
    {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
    }
}
