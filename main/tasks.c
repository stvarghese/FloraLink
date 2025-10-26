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

#include "onboardled.h"
#include "distance.h"
#include "monitor.h"
#include "gpiobutton.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "webserver.h"
#include "websockserver.h"
#include "wifi_setup.h"
#include "nodeio.h"
#include "nvm.h"
#include "dns_server.h"
#include "modemanager.h"

#include "commonutils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FloraLink";

/**
 * @brief Task to manage LED status indication.
 *
 * Provides visual feedback for system status using different LED patterns.
 * Uses non-blocking patterns for efficient resource usage.
 * @param pvParameters Unused
 */
static void led_task(void *pvParameters)
{
    ESP_LOGI(TAG, "LED task started, on core %d", xPortGetCoreID());

    // State tracking for efficient updates
    bool last_wifi_state = false;
    int last_node_count = -1;

    while (1)
    {
        // Wait for active mode (blocks during idle phase)
        xEventGroupWaitBits(g_mode_event_group,
                            MODE_ACTIVE_BIT,
                            pdFALSE,        // Don't clear on exit
                            pdTRUE,         // Wait for all bits
                            portMAX_DELAY); // Wait forever

        ESP_LOGD(TAG, "LED task entering active phase");

        // Active phase loop - runs while active bit is set
        while (xEventGroupGetBits(g_mode_event_group) & MODE_ACTIVE_BIT)
        {
            // Get current system status
            bool wifi_connected = wifi_is_connected();
            int node_count = nodeio_get_connected_node_count();

            // Check if status changed
            bool status_changed = (wifi_connected != last_wifi_state ||
                                   node_count != last_node_count);

            // Stop current pattern if status changed
            if (status_changed || onboardled_is_pattern_running())
            {
                onboardled_stop_pattern();
            }

            // Start appropriate pattern if none running
            if (!onboardled_is_pattern_running())
            {
                if (!wifi_connected)
                {
                    onboardled_start_blink(250, 250, 0, &ONBOARDLED_COLOR_ORANGE);
                }
                else if (node_count == 0)
                {
                    onboardled_start_heartbeat(0, 100, 300, 3000, &ONBOARDLED_COLOR_GREEN);
                }
                else
                {
                    onboardled_start_heartbeat(0, 100, 300, 2000, &ONBOARDLED_COLOR_BLUE);
                }
            }

            // Update state tracking
            last_wifi_state = wifi_connected;
            last_node_count = node_count;

            // 3s delay during active phase only
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        ESP_LOGD(TAG, "LED task exiting active phase - stopping any running patterns");

        // Stop any running LED patterns when exiting active phase
        if (onboardled_is_pattern_running())
        {
            onboardled_stop_pattern();
        }
    }
}

/**
 * @brief Task to periodically measure distance and log the result.
 *
 * Uses distance_measure() to read the ultrasonic sensor every 500ms during active phase only.
 * Completely suspends during idle phase to save power.
 * @param pvParameters Unused
 */
static void distance_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Distance task started, on core %d", xPortGetCoreID());

    while (1)
    {
        // Wait for active mode (blocks during idle phase)
        xEventGroupWaitBits(g_mode_event_group,
                            MODE_ACTIVE_BIT,
                            pdFALSE,        // Don't clear on exit
                            pdTRUE,         // Wait for all bits
                            portMAX_DELAY); // Wait forever

        ESP_LOGD(TAG, "Distance task entering active phase");

        // Active phase loop - runs while active bit is set
        while (xEventGroupGetBits(g_mode_event_group) & MODE_ACTIVE_BIT)
        {
            uint32_t distance = 0;
            esp_err_t measure_result = distance_measure(400, &distance);
            if (measure_result == ESP_OK)
            {
                distance_publish(PUB_WEBSERVER, distance);
            }
            else
            {
                distance_publish_err(PUB_LOG, measure_result);
                distance_publish_err(PUB_WEBSERVER, measure_result);
            }

            // 500ms delay during active phase only
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        ESP_LOGD(TAG, "Distance task exiting active phase");
    }
}

void monitor_task_1s(void *arg)
{
    ESP_LOGI(TAG, "monitor_task_1s started, on core %d", xPortGetCoreID());

    while (1)
    {
        // Wait for active mode (blocks during idle phase)
        xEventGroupWaitBits(g_mode_event_group,
                            MODE_ACTIVE_BIT,
                            pdFALSE,        // Don't clear on exit
                            pdTRUE,         // Wait for all bits
                            portMAX_DELAY); // Wait forever

        ESP_LOGD(TAG, "Monitor task entering active phase");

        // Active phase loop - runs while active bit is set
        while (xEventGroupGetBits(g_mode_event_group) & MODE_ACTIVE_BIT)
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

            // Webserver health monitor every 30 seconds
            static int webserver_health_counter = 0;
            if (++webserver_health_counter >= 30)
            {
                webserver_health_monitor();
                webserver_health_counter = 0;
            }

            // modemanager_dump_pm_locks();

            HEAP_TRACE_END_TIMER(); // Monitor task includes ping operations which allocate memory

            // 1s delay during active phase only
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        ESP_LOGD(TAG, "Monitor task exiting active phase");
    }
}

void input_process_task(void *arg)
{
    ESP_LOGI(TAG, "input_process_task started, on core %d", xPortGetCoreID());

    while (1)
    {
        // Wait for active mode (blocks during idle phase)
        xEventGroupWaitBits(g_mode_event_group,
                            MODE_ACTIVE_BIT,
                            pdFALSE,        // Don't clear on exit
                            pdTRUE,         // Wait for all bits
                            portMAX_DELAY); // Wait forever

        ESP_LOGD(TAG, "Input process task entering active phase");

        // Process events frequently during active phase
        while (xEventGroupGetBits(g_mode_event_group) & MODE_ACTIVE_BIT)
        {
            // Process any pending button events (non-blocking)
            process_gpiobutton_events();

            // Active mode: frequent processing for responsiveness
            vTaskDelay(pdMS_TO_TICKS(10)); // 10ms during active phase
        }

        ESP_LOGD(TAG, "Input process task exiting active phase");
    }
}

void monitor_task_rmt(void *arg)
{
    ESP_LOGI(TAG, "monitor_task_rmt started, on core %d", xPortGetCoreID());

    while (1)
    {
        // Wait for active mode (blocks during idle phase)
        xEventGroupWaitBits(g_mode_event_group,
                            MODE_ACTIVE_BIT,
                            pdFALSE,        // Don't clear on exit
                            pdTRUE,         // Wait for all bits
                            portMAX_DELAY); // Wait forever

        ESP_LOGD(TAG, "RMT monitor task entering active phase");

        // Active phase loop - runs while active bit is set
        while (xEventGroupGetBits(g_mode_event_group) & MODE_ACTIVE_BIT)
        {
            // Process RMT events during active phase only
            monitor_process_rmt_rx();
        }

        ESP_LOGD(TAG, "RMT monitor task exiting active phase");
    }
}

/**
 * @brief Activity notification task for ISR-safe mode management.
 *
 * This high-priority task handles activity notifications from ISRs
 * and triggers the mode manager to enter active mode.
 */
void activity_notification_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Activity notification task started, on core %d", xPortGetCoreID());

    while (1)
    {
        // Wait for notification from ISR (blocks indefinitely)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Activity detected - enter active mode
        ESP_LOGD(TAG, "Activity notification received from ISR");
        modemanager_enter_active_auto();
    }
}

/**
 * @brief DNS server task for captive portal functionality.
 *
 * Provides DNS hijacking to redirect all requests to the AP IP address
 * during WiFi configuration mode. This task is created conditionally
 * when in AP mode.
 */
void dns_server_task(void *pvParameters)
{
    // DNS server implementation delegated to dns_server module
    dns_server_run(); // This function contains the actual DNS task logic
}

/**
 * @brief DNS server task creation function.
 *
 * Creates the DNS server task and starts the DNS server.
 */
void dns_server_create_task(void)
{
    xTaskCreate(dns_server_task, "dns_server", 2048, NULL, 5, NULL);
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

    // Configure auto light sleep for power management
    if (modemanager_init_auto_light_sleep() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure auto light sleep - continuing anyway");
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
    esp_log_level_set("onboardled", ESP_LOG_DEBUG);
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("WiFiSetup", ESP_LOG_DEBUG);
    esp_log_level_set("ModeManager", ESP_LOG_DEBUG);
    esp_log_level_set("FloraLink", ESP_LOG_DEBUG);
    // esp_log_level_set("monitor", ESP_LOG_DEBUG);
    blink_init();

    // Run onboard LED test pattern to verify functionality
    // onboardled_test_pattern();

    // Show off RGB capabilities with a colorful dance
    // ESP_LOGD(TAG, "LED color dance starting...");
    // onboardled_dance(2, 80, 40); // 2 cycles, 80ms on, 40ms off per color
    // ESP_LOGD(TAG, "LED color dance complete");

    monitor_init();
    gpiobutton_init(WIFI_RESET_PIN, wifi_reset_button_cb);
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(distance_task, "distance_task", 8192, NULL, 5, NULL);
    xTaskCreate(monitor_task_1s, "monitor_task_1s", 4096, NULL, 5, NULL);
    xTaskCreate(monitor_task_rmt, "monitor_task_rmt", 4096, NULL, 5, NULL);
    xTaskCreate(input_process_task, "input_process_task", 2048, NULL, 10, NULL);

    // Create activity notification task for ISR-safe notifications (high priority)
    TaskHandle_t activity_task_handle;
    xTaskCreate(activity_notification_task, "activity_notify", 4096, NULL, 15, &activity_task_handle);
    modemanager_set_activity_task_handle(activity_task_handle);

    // Enter active phase to start normal operation (auto sleep version)
    modemanager_enter_active_auto();
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
