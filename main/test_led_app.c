#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "onboardled.h"

static const char *TAG = "test_led_app";

#ifndef TEST_BUTTON_GPIO
// Default to the common BOOT button on many ESP boards; change as needed
#define TEST_BUTTON_GPIO 9
#endif

static SemaphoreHandle_t s_button_sem = NULL;
static TaskHandle_t s_demo_task = NULL;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

static void demo_task(void *arg)
{
    ESP_LOGI(TAG, "Demo task started");

    if (onboardled_start_breathing(750, 1250, 1000, 3, &ONBOARDLED_COLOR_BLUE))
    {
        /* Wait for breathing to finish (can be interrupted via onboardled_stop_pattern()) */
        while (onboardled_is_pattern_running())
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "Breathing demo complete — running follow-up patterns");

        onboardled_quick_flash(3, NULL);
        vTaskDelay(pdMS_TO_TICKS(200));

        ESP_LOGI(TAG, "Heartbeat x3");
        onboardled_heartbeat(3, 120, 300, 400, &ONBOARDLED_COLOR_CYAN);

        ESP_LOGI(TAG, "Success then failure patterns");
        onboardled_success(NULL);
        vTaskDelay(pdMS_TO_TICKS(300));
        onboardled_failure(NULL);

        ESP_LOGI(TAG, "Dance sequence (4 cycles)");
        onboardled_dance(4, 150, 80);
    }
    else
    {
        ESP_LOGW(TAG, "Could not start breathing (pattern busy)");
    }

    ESP_LOGI(TAG, "Demo task complete");
    s_demo_task = NULL;
    vTaskDelete(NULL);
}

static void led_button_task(void *arg)
{
    bool breathing_on = false;

    ESP_LOGI(TAG, "LED breathing test task started, button on GPIO %d", TEST_BUTTON_GPIO);

    for (;;)
    {
        if (xSemaphoreTake(s_button_sem, portMAX_DELAY) == pdTRUE)
        {
            // simple debounce
            vTaskDelay(pdMS_TO_TICKS(50));
            int level = gpio_get_level(TEST_BUTTON_GPIO);
            if (level == 0)
            {
                /* Start demo in separate task so the button handler remains responsive.
                 * If a demo is already running, stop it immediately.
                 */
                if (s_demo_task != NULL)
                {
                    ESP_LOGI(TAG, "Button pressed: stopping running demo/patterns");
                    onboardled_stop_pattern();
                    /* Delete the demo task to ensure it doesn't continue blocking the button task.
                     * Deleting a task from another task is allowed in FreeRTOS for this test harness.
                     */
                    vTaskDelete(s_demo_task);
                    s_demo_task = NULL;
                    breathing_on = false;
                }
                else
                {
                    ESP_LOGI(TAG, "Button pressed: starting demo task (breathing + follow-up)");
                    if (xTaskCreate(demo_task, "led_demo_task", 4096, NULL, 4, &s_demo_task) != pdPASS)
                    {
                        ESP_LOGW(TAG, "Failed to start demo task");
                        s_demo_task = NULL;
                    }
                }

                // Small delay to avoid retriggering too quickly
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }
}

void app_main(void)
{
    esp_log_level_set("ONBOARDLED", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "Starting LED pattern test app");

    // Initialize onboard LED (uses CONFIG_BLINK_GPIO or default in module)
    onboardled_begin(CONFIG_BLINK_GPIO, false);

    // Create semaphore for button
    s_button_sem = xSemaphoreCreateBinary();
    if (s_button_sem == NULL)
    {
        ESP_LOGE(TAG, "Failed to create button semaphore");
        return;
    }

    // Configure button GPIO
    gpio_reset_pin(TEST_BUTTON_GPIO);
    gpio_set_direction(TEST_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(TEST_BUTTON_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(TEST_BUTTON_GPIO, GPIO_INTR_NEGEDGE);

    // Install ISR service
    gpio_install_isr_service(0);
    gpio_isr_handler_add(TEST_BUTTON_GPIO, button_isr_handler, s_button_sem);

    // Start the button handler task
    xTaskCreate(led_button_task, "led_button_task", 4096, NULL, 5, NULL);
}
