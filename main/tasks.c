
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

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
    ESP_LOGI(TAG, "LED task started");
    while (1)
    {
        blink_toggle();
        vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
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
    ESP_LOGI(TAG, "Distance task started");
    while (1)
    {
        uint32_t distance = 0;
        esp_err_t measure_result = distance_measure(400, &distance);
        if (measure_result == ESP_OK)
        {
            ESP_LOGI(TAG, "Measured distance: %d cm", distance);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to measure distance: %s, code: 0x%X", esp_err_to_name(measure_result), measure_result);
        }
        // Generate a test pulse for RMT monitor (4us low, 10us high)
        // misc_test_function();
        vTaskDelay(2000 / portTICK_PERIOD_MS);
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
    ESP_LOGI(TAG, "Init task started");
    if (distance_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize distance sensor");
        vTaskDelete(NULL);
    }
    blink_init();
    monitor_init();
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(distance_task, "distance_task", 8192, NULL, 5, NULL);
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
