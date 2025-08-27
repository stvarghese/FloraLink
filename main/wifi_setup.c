/**
 * @file wifi_setup.c
 * @brief WiFi initialization and management for FloraLink ESP32 project.
 *
 * This module handles WiFi station mode setup, connection, and exposes the current SSID.
 *
 * Usage:
 * 1. Call wifi_setup() during system initialization.
 * 2. Use wifi_get_ssid() to retrieve the connected SSID for display or diagnostics.
 */

#include "wifi_setup.h"
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/event_groups.h>
#include "lwip/ip4_addr.h"
#include "sdkconfig.h"

#define WIFI_SSID CONFIG_WIFI_SSID
#define WIFI_PASS CONFIG_WIFI_PASS
#define WIFI_MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0 ///< Event bit for successful connection
#define WIFI_FAIL_BIT BIT1      ///< Event bit for connection failure

// Holds the SSID of the currently configured WiFi network
static char s_current_ssid[WIFI_SSID_MAX_LEN] = {0};
// Logging tag for ESP-IDF logging macros
static const char *TAG = "WiFiSetup";
// Retry counter for connection attempts
static int s_retry_num = 0;
// FreeRTOS event group to signal connection events
static EventGroupHandle_t s_wifi_event_group;

/**
 * @brief Event handler for WiFi and IP events.
 *
 * Handles WiFi start, disconnect, and IP acquisition events.
 * - On start: attempts to connect.
 * - On disconnect: retries up to WIFI_MAX_RETRY, then signals failure.
 * - On IP: signals successful connection.
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // WiFi started, attempt to connect
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // Disconnected: retry or signal failure
        if (s_retry_num < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        // Got IP: signal success
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: %s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Initialize WiFi in station mode and connect to the configured SSID.
 *
 * This function sets up the WiFi driver, registers event handlers, and attempts to connect
 * to the WiFi network specified by WIFI_SSID and WIFI_PASS. It blocks until connection or failure.
 *
 * @return ESP_OK on successful connection, ESP_FAIL otherwise.
 */
esp_err_t wifi_setup(void)
{
    // 1. Create event group for connection events
    s_wifi_event_group = xEventGroupCreate();

    // 2. Initialize NVS, TCP/IP, and event loop
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. Create default WiFi station
    esp_netif_create_default_wifi_sta();

    // 4. Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Register event handlers for WiFi and IP events
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 6. Configure WiFi connection parameters
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    // Store SSID for later retrieval
    strncpy(s_current_ssid, (const char *)wifi_config.sta.ssid, WIFI_SSID_MAX_LEN - 1);
    s_current_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    // 7. Set WiFi mode and apply configuration
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // 8. Wait for connection or failure event
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    // 9. Return result
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
        return ESP_OK;
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}

/**
 * @brief Get the SSID of the currently configured WiFi network.
 *
 * @return Pointer to the SSID string.
 */
const char *wifi_get_ssid(void)
{
    return s_current_ssid;
}
