/**
 * @file tasks.c
 * @brief Main application task management for the ESP32 project.
 *
 * This file defines and launches the FreeRTOS tasks that make up the application:
 * - LED blinking
 * - Ultrasonic distance measurement
 * - RMT monitoring (via monitor module)
 *
 * Each task is responsible for a specific function and runs independently under FreeRTOS.
 * The init task performs system initialization and launches the other tasks.
 *
 * Task Overview:
 * - led_task: Toggles the LED at a configurable interval.
 * - distance_task: Periodically measures distance and logs the result.
 * - monitor_task: (see monitor.c) Handles RMT event logging.
 *
 * FreeRTOS Integration:
 * - Tasks are created with xTaskCreate or xTaskCreatePinnedToCore.
 * - vTaskDelay is used for periodic scheduling.
 * - All initialization is performed in init_task, which deletes itself after setup.
 */

#include "blink.h"
#include "distance.h"
#include "monitor.h"
#include "gpiobutton.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_cpu.h"

#include "webserver.h"
#include "websockserver.h"
#include "wifi_setup.h"
#include "nodeio.h"
#include "nvm.h"
#include "commonutils.h"

static const char *TAG = "FloraLink";

/**
 * @brief Task to periodically toggle the LED.
 *
 * Uses blink_toggle() to change the LED state at the interval specified by CONFIG_BLINK_PERIOD.
 * Runs indefinitely.
 * @param pvParameters Unused
 */
static void led_task(void *pvParameters)
{
    ESP_LOGI(TAG, "LED task started, on core %d", xPortGetCoreID());
    while (1)
    {
        blink_toggle();
        vTaskDelay(blink_get_period_ms() / portTICK_PERIOD_MS);
    }
}

/**
 * @brief Task to periodically measure distance and log the result.
 *
 * Uses distance_measure() to read the ultrasonic sensor every 2 seconds.
 * Also generates a test pulse on GPIO 4 for RMT monitoring.
 * @param pvParameters Unused
 */
static void distance_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Distance task started, on core %d", xPortGetCoreID());
    while (1)
    {
        uint32_t distance = 0;
        esp_err_t measure_result = distance_measure(400, &distance);
        if (measure_result == ESP_OK)
        {
            // distance_publish(PUB_LOG, distance);
            distance_publish(PUB_WEBSERVER, distance);
        }
        else
        {
            distance_publish_err(PUB_LOG, measure_result);
            distance_publish_err(PUB_WEBSERVER, measure_result);
        }
        // Generate a test pulse for RMT monitor (4us low, 10us high)
        // misc_test_function();
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void monitor_task_1s(void *arg)
{
    ESP_LOGI(TAG, "monitor_task_1s started, on core %d", xPortGetCoreID());
    while (1)
    {
        HEAP_TRACE_START("MONITOR_1S");

        // Update CPU load even if no RMT event
        monitor_update_cpu_load();
        nodeio_monitor_nodeslist();
        // Ping every 5 seconds
        static int ping_counter = 0;
        if (++ping_counter >= 5)
        {
            nodeio_active_nodes_ping();
            ping_counter = 0;
        }
        nodeio_process_subscription_updates();

        HEAP_TRACE_END_TIMER(); // Monitor task includes ping operations which allocate memory

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void input_process_task(void *arg)
{
    ESP_LOGI(TAG, "input_process_task started, on core %d", xPortGetCoreID());
    while (1)
    {
        process_gpiobutton_events();
        // Always yield to avoid watchdog
        vTaskDelay(1);
    }
}

void monitor_task_rmt(void *arg)
{
    ESP_LOGI(TAG, "monitor_task_rmt started, on core %d", xPortGetCoreID());
    while (1)
    {
        monitor_process_rmt_rx();
    }
}

/**
 * @brief Initialization task for the application.
 *
 * Initializes all modules (distance, blink, monitor) and launches the main tasks.
 * Deletes itself after setup is complete.
 * @param pvParameters Unused
 */
static void init_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Init task started on core %d", xPortGetCoreID());
    // ESP_LOGI(TAG, "Number of cores: %d", esp_cpu_get_core_count());

    // Initialize NVM for all modules
    if (nvm_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize NVM storage");
        vTaskDelete(NULL);
    }

    if (wifi_setup() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi");
        vTaskDelete(NULL);
    }
    if (distance_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize distance sensor");
        vTaskDelete(NULL);
    }
    if (webserver_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start webserver");
        vTaskDelete(NULL);
    }
    if (nodeio_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize nodeio");
        vTaskDelete(NULL);
    }

// esp_log_level_set("httpd_ws", ESP_LOG_DEBUG);
// esp_log_level_set("httpd_txrx", ESP_LOG_DEBUG);
#ifdef HEAP_TRACING_ENABLED
    esp_log_level_set("PING", ESP_LOG_DEBUG);
    esp_log_level_set("WS_PING", ESP_LOG_DEBUG);
    esp_log_level_set("RESET_PONG", ESP_LOG_DEBUG);
    esp_log_level_set("PONG_TIMEOUT", ESP_LOG_DEBUG);
    esp_log_level_set("MONITOR_1S", ESP_LOG_DEBUG);
    esp_log_level_set("HEALTH_MONITOR", ESP_LOG_DEBUG);
    esp_log_level_set("WS_RX", ESP_LOG_DEBUG);
    esp_log_level_set("WS_SEND", ESP_LOG_DEBUG);
    esp_log_level_set("SESSION_UPDATE", ESP_LOG_DEBUG);
    esp_log_level_set("SESSION_REMOVE", ESP_LOG_DEBUG);
    esp_log_level_set("NODESLIST", ESP_LOG_DEBUG);
#endif
    blink_init();
    monitor_init();
    gpiobutton_init(WIFI_RESET_PIN, wifi_reset_button_cb);
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(distance_task, "distance_task", 8192, NULL, 5, NULL);
    xTaskCreate(monitor_task_1s, "monitor_task_1s", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_task_rmt, "monitor_task_rmt", 4096, NULL, 5, NULL);
    xTaskCreate(input_process_task, "input_process_task", 2048, NULL, 10, NULL);
    vTaskDelete(NULL);
}

/**
 * @brief Main entry point for the application.
 *
 * Launches the init_task, which sets up all other tasks and modules.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "app_main started");
    xTaskCreate(init_task, "init_task", 4096, NULL, 10, NULL);
}
