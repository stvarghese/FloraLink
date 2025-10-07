#include "modemanager.h"

#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <driver/gpio.h>

#include "gpiobutton.h" // For button pin reference
#include "wifi_setup.h" // For WiFi configuration functions
#include "onboardled.h" // For LED status indication
#include "monitor.h"    // For RMT power management

#define ACTIVE_WINDOW_DEFAULT_MS 30000 // 30s default, configurable
#define WAKEUP_GPIO_PIN 9              // Onboard flash/boot button (GPIO9 on ESP32-C3 dev boards)

static const char *TAG = "ModeManager";
static TimerHandle_t active_window_timer = NULL;
static volatile int is_active = 0;
static uint32_t active_window_ms = ACTIVE_WINDOW_DEFAULT_MS;

// Event group for mode signaling
EventGroupHandle_t g_mode_event_group = NULL;

// Task handle for ISR notifications
static TaskHandle_t activity_notification_task_handle = NULL;

// ===== AUTO LIGHT SLEEP INITIALIZATION =====

esp_err_t modemanager_init_auto_light_sleep(void)
{
    // Enable power management debug logging for multiple components
    esp_log_level_set("pm", ESP_LOG_DEBUG);
    esp_log_level_set("esp_pm", ESP_LOG_DEBUG);
    esp_log_level_set("sleep", ESP_LOG_DEBUG);
    esp_log_level_set("esp_sleep", ESP_LOG_DEBUG);
    esp_log_level_set("pm_esp32c3", ESP_LOG_DEBUG);

    // Create event group for mode signaling
    g_mode_event_group = xEventGroupCreate();
    if (!g_mode_event_group)
    {
        ESP_LOGE(TAG, "Failed to create mode event group");
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Mode event group created");

    // Activity notification task will be created centrally in tasks.c
    ESP_LOGI(TAG, "Activity notification task will be created by central task manager");

    // Configure auto light sleep for power management
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP32C3_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 10,        // Minimum frequency for power saving
        .light_sleep_enable = true // Enable automatic light sleep
    };

    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Auto light sleep enabled (max: %d MHz, min: %d MHz)",
                 pm_config.max_freq_mhz, pm_config.min_freq_mhz);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to configure auto light sleep: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

// ===== MANUAL SLEEP FUNCTIONS (Legacy) =====

static void active_window_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Active window expired, entering low-power mode");
    modemanager_exit_active();
}

static void modemanager_prepare_for_sleep_and_wakeup(void)
{
    extern volatile int g_is_light_sleep;
    g_is_light_sleep = 1;

    // Configure WiFi for sleep mode - longer listen interval for better power savings
    wifi_configure_sleep_mode();

    // Configure the GPIO for sleep wakeup
    gpiobutton_configure_sleep_wakeup(WAKEUP_GPIO_PIN);

    // wait 10s
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Enable GPIO wakeup for ESP32-C3 (low level triggers wake)
    esp_err_t ret = gpio_wakeup_enable(WAKEUP_GPIO_PIN, GPIO_INTR_LOW_LEVEL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO wakeup enable failed: %s", esp_err_to_name(ret));
        return;
    }

    // Register GPIO as wakeup source
    esp_sleep_enable_gpio_wakeup();

    // Enable WiFi wakeup to maintain connection
    esp_sleep_enable_wifi_wakeup();

    ESP_LOGI(TAG, "Configured GPIO %d and WiFi wakeup for ESP32-C3", WAKEUP_GPIO_PIN);
}
void modemanager_enter_active(void)
{
    extern volatile int g_is_light_sleep;
    if (!is_active)
    {
        ESP_LOGI(TAG, "Entering ACTIVE window");
        is_active = 1;
        g_is_light_sleep = 0;

        // Configure WiFi for active mode - shorter listen interval for maximum responsiveness
        wifi_configure_active_mode();
    }
    // Start or reset timer
    if (!active_window_timer)
    {
        active_window_timer = xTimerCreate("active_window", pdMS_TO_TICKS(active_window_ms), pdFALSE, NULL, active_window_timer_cb);
        // Do not enter sleep here; sleep is handled when the active window expires
    }
    if (active_window_timer)
    {
        xTimerStop(active_window_timer, 0);
        xTimerChangePeriod(active_window_timer, pdMS_TO_TICKS(active_window_ms), 0);
        xTimerStart(active_window_timer, 0);
    }
    // // Ensure normal FreeRTOS scheduling resumes if we were in light sleep
    // vTaskDelay(1);
}

void modemanager_exit_active(void)
{
    if (is_active)
    {
        ESP_LOGI(TAG, "Exiting ACTIVE window, entering low-power mode");
        is_active = 0;
    }
    if (active_window_timer)
    {
        xTimerStop(active_window_timer, 0);
    }

    // Prepare for and enter light sleep
    modemanager_prepare_for_sleep_and_wakeup();
    ESP_LOGI(TAG, "Entering ESP32 light sleep (waiting for GPIO/WiFi wakeup)...");

    // Stop any running LED patterns before sleep
    onboardled_stop_pattern();

    // Use a blocking pattern to indicate entering light sleep - completes before sleep
    onboardled_signal_code(3, 150, 100, 500, &ONBOARDLED_COLOR_CYAN); // 3 cyan flashes = "going to sleep"

    vTaskDelay(pdMS_TO_TICKS(50)); // Allow logs to flush and LED pattern to complete
    esp_light_sleep_start();

    // Check what woke us up
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO)
    {
        ESP_LOGI(TAG, "Woke from light sleep by GPIO %d", WAKEUP_GPIO_PIN);
    }
    else
    {
        ESP_LOGI(TAG, "Woke from light sleep by cause %d", cause);
    }

    // signal wakeup with LED pattern
    onboardled_signal_code(2, 150, 100, 500, &ONBOARDLED_COLOR_GREEN); // 2 green flashes = "woke up"

    // On wake, the system resumes here
    modemanager_enter_active(); // Re-enter active window on wake
}

void modemanager_notify_activity(void)
{
    // Call this on any node data or user action
    modemanager_enter_active();
}

int modemanager_is_active(void)
{
    return is_active;
}

void modemanager_light_sleep(void)
{
    ESP_LOGI(TAG, "Entering light sleep mode");
    esp_light_sleep_start();
}

void modemanager_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep mode");
    esp_deep_sleep_start();
}

// ===== AUTO SLEEP FUNCTIONS (New approach) =====

static void active_window_timer_auto_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Active window expired, entering auto sleep idle mode");
    modemanager_exit_active_auto();
}

void modemanager_enter_active_auto(void)
{
    extern volatile int g_is_light_sleep;
    if (!is_active)
    {
        ESP_LOGI(TAG, "Entering ACTIVE window (auto sleep mode)");
        is_active = 1;
        g_is_light_sleep = 0;

        // Configure WiFi for active mode - shorter listen interval for maximum responsiveness
        wifi_configure_active_mode();

        // temporarily disable all
        //  Resume RMT monitoring for full functionality during active periods
        monitor_resume_rmt();

        // print empty line for readability
        printf("\n");
        // PM lock state dump after entering active phase
        ESP_LOGI(TAG, "PM lock state on entering active phase:");
        modemanager_dump_pm_locks();

        // print empty line for readability
        printf("\n");

        // Set active bit to wake up all waiting tasks
        if (g_mode_event_group)
        {
            xEventGroupSetBits(g_mode_event_group, MODE_ACTIVE_BIT);
            ESP_LOGD(TAG, "Active mode event bit set");
        }
    }

    // Start or reset timer - same timer logic as manual version
    if (!active_window_timer)
    {
        active_window_timer = xTimerCreate("active_window_auto", pdMS_TO_TICKS(active_window_ms),
                                           pdFALSE, NULL, active_window_timer_auto_cb);
    }
    if (active_window_timer)
    {
        xTimerStop(active_window_timer, 0);
        xTimerChangePeriod(active_window_timer, pdMS_TO_TICKS(active_window_ms), 0);
        xTimerStart(active_window_timer, 0);
    }
}

void modemanager_exit_active_auto(void)
{
    if (is_active)
    {
        ESP_LOGI(TAG, "Exiting ACTIVE window, entering auto sleep idle mode");
        is_active = 0;

        // Clear active bit to put all tasks into idle mode
        if (g_mode_event_group)
        {
            xEventGroupClearBits(g_mode_event_group, MODE_ACTIVE_BIT);
            ESP_LOGD(TAG, "Active mode event bit cleared");
        }
    }
    if (active_window_timer)
    {
        xTimerStop(active_window_timer, 0);
    }

    extern volatile int g_is_light_sleep;
    g_is_light_sleep = 1;

    // Configure WiFi for sleep mode - longer listen interval for better power savings
    wifi_configure_sleep_mode();

    // Stop any running LED patterns before entering idle mode
    onboardled_stop_pattern();

    // print empty line for readability
    printf("\n");
    // Suspend RMT monitoring to release APB_FREQ_MAX lock for better sleep
    monitor_suspend_rmt();

    // Signal entering idle mode with LED pattern
    onboardled_signal_code(2, 100, 50, 300, &ONBOARDLED_COLOR_BLUE); // 2 blue flashes = "entering idle"

    // start breathing pattern: onboardled_start_breathing(uint32_t fade_in_ms, uint32_t fade_out_ms, uint32_t pause_ms, uint32_t cycles, onboardled_color_t *color)
    onboardled_start_breathing(750, 250, 2000, 3, &ONBOARDLED_COLOR_BLUE);

    // No explicit sleep call - auto light sleep will handle power management
    // Tasks will automatically sleep during vTaskDelay() and idle periods
    ESP_LOGI(TAG, "Auto sleep idle mode active - CPU will sleep automatically during idle periods");

    // PM lock state dump
    ESP_LOGI(TAG, "PM lock state on entering auto sleep:");
    modemanager_dump_pm_locks();

    // print empty line for readability
    printf("\n");
}

void modemanager_notify_activity_auto(void)
{
    // Call this on any node data or user action - same as manual version
    modemanager_enter_active_auto();
}

// ISR-safe activity notification using task notification
void IRAM_ATTR modemanager_notify_wkup_activity_from_isr(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (activity_notification_task_handle != NULL)
    {
        // Send notification to activity task from ISR
        vTaskNotifyGiveFromISR(activity_notification_task_handle, &xHigherPriorityTaskWoken);

        // Request context switch if higher priority task was woken
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Task to handle activity notifications from ISR
void modemanager_set_activity_task_handle(TaskHandle_t task_handle)
{
    activity_notification_task_handle = task_handle;
    ESP_LOGD(TAG, "Activity task handle set");
}

// Dump pm locks
void modemanager_dump_pm_locks(void)
{
    // Dump power management locks preventing sleep to the specified output stream
    esp_pm_dump_locks(stdout);
}
