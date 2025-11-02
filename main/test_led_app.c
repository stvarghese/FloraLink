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

static void IRAM_ATTR button_isr_handler(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
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
                // Toggle breathing pattern on each press
                if (!breathing_on)
                {
                    ESP_LOGI(TAG, "Starting breathing (1000ms in/out, 2000ms pause)");
                    // start infinite breathing
                    if (onboardled_start_breathing(1000, 1000, 2000, 0, &ONBOARDLED_COLOR_BLUE))
                    {
                        breathing_on = true;
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Could not start breathing (pattern busy)");
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "Stopping breathing");
                    onboardled_stop_pattern();
                    breathing_on = false;
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
