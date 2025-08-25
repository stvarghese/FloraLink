
/**
 * @file monitor.c
 * @brief RMT monitoring and event logging module.
 *
 * This module configures the ESP32 RMT peripheral to monitor digital pulses on a GPIO pin,
 * typically for debugging or level shifter testing. It uses FreeRTOS tasks and queues to
 * safely transfer RMT events from ISR context to a logging task.
 *
 * Configuration:
 * - The monitored GPIO is set via CONFIG_GPIO_MONITOR_INPUT_PIN in menuconfig or sdkconfig.
 * - RMT is set up for RX mode with a 10 MHz clock (0.1 us per tick).
 * - Buffer size, filtering, and event queue are all configurable in this file.
 *
 * Steps:
 * 1. Configure the monitored GPIO as input with pulldown.
 * 2. Configure a test output GPIO (e.g., GPIO 4) for generating pulses.
 * 3. Set up the RMT RX channel and register the receive-done callback.
 * 4. Allocate a DMA-capable buffer for RMT symbols.
 * 5. Enable RMT and arm it for reception.
 * 6. Create a FreeRTOS queue for RMT events and a task to process them.
 * 7. In the callback, push event data to the queue (ISR-safe, no logging).
 * 8. In the task, log pulse timings and re-arm the RMT for the next event.
 */

#include "monitor.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "monitor";
static QueueHandle_t s_rmt_evt_q;
static rmt_channel_handle_t g_rx_chan = NULL;
static void *g_rx_buf = NULL;
static size_t g_rx_buf_sz = 0;
static rmt_receive_config_t g_rx_cfg;

/**
 * @brief RMT RX done callback (ISR context).
 *
 * This function is called by the RMT driver when a receive event completes.
 * It pushes the event data to a FreeRTOS queue for processing in a task context.
 * No logging or heavy processing is done here (ISR must be fast).
 *
 * Detailed steps:
 * 1. Prepare a variable to track if a higher-priority task is woken by the queue send.
 * 2. Use xQueueSendFromISR to send the event data (by value) to the queue.
 *    - This is ISR-safe and avoids race conditions.
 *    - If the queue is full, the event is dropped (consider monitoring for overflows).
 * 3. Return whether a context switch should occur after the ISR (if a higher-priority task was woken).
 *
 * @param chan RMT channel handle (unused here)
 * @param edata Pointer to event data (copied into queue)
 * @param user_ctx Unused
 * @return true if a higher-priority task was woken and a context switch is needed
 */
static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t chan,
                                     const rmt_rx_done_event_data_t *edata,
                                     void *user_ctx)
{
    // 1. Track if a higher-priority task is woken by the queue operation
    BaseType_t hp_task_woken = pdFALSE;

    // 2. Send the event data to the queue (by value, ISR-safe)
    //    - This allows the main monitor task to process the event outside the ISR
    //    - Never do logging or heavy work here!
    xQueueSendFromISR(s_rmt_evt_q, edata, &hp_task_woken);

    // 3. Return whether a context switch should occur after the ISR
    //    - If a higher-priority task was waiting on the queue, it will run next
    return hp_task_woken == pdTRUE;
}

/**
 * @brief FreeRTOS task to process and log RMT RX events.
 *
 * Waits for events from the ISR via the queue, logs pulse timings, and re-arms the RMT.
 * This is the only place where logging and heavy processing should occur.
 * @param arg Unused
 */
static void monitor_task(void *arg)
{
    rmt_rx_done_event_data_t evt;
    while (1)
    {
        if (xQueueReceive(s_rmt_evt_q, &evt, portMAX_DELAY))
        {
            const rmt_symbol_word_t *syms = evt.received_symbols;
            for (size_t i = 0; i < evt.num_symbols; i++)
            {
                float t0_us = syms[i].duration0 / 10.0f;
                float t1_us = syms[i].duration1 / 10.0f;
                ESP_LOGI(TAG, "lvl0=%d t0=%.1fus | lvl1=%d t1=%.1fus",
                         syms[i].level0, t0_us, syms[i].level1, t1_us);
            }
            // Re-arm RMT for next event
            rmt_receive(g_rx_chan, g_rx_buf, g_rx_buf_sz, &g_rx_cfg);
        }
    }
}

/**
 * @brief Initialize the RMT monitor module.
 *
 * Configures the monitored GPIO, RMT RX channel, event queue, and processing task.
 * Call this once during system initialization.
 */
void monitor_init(void)
{
    ESP_LOGI(TAG, "Configuring RMT to read TRIG pin output pulse on GPIO %d", CONFIG_GPIO_MONITOR_INPUT_PIN);
    // 1. Configure monitored GPIO as input with pulldown
    gpio_reset_pin(CONFIG_GPIO_MONITOR_INPUT_PIN);
    gpio_set_direction(CONFIG_GPIO_MONITOR_INPUT_PIN, GPIO_MODE_INPUT);
    gpio_pulldown_en(CONFIG_GPIO_MONITOR_INPUT_PIN);
    // 2. Configure test output GPIO (optional)
    gpio_reset_pin(4);
    gpio_set_direction(4, GPIO_MODE_OUTPUT);
    gpio_set_level(4, 0);
    // 3. Set up RMT RX channel
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = CONFIG_GPIO_MONITOR_INPUT_PIN,
        .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz (0.1 us per tick)
        .mem_block_symbols = 64,
        .intr_priority = 0,
        .flags = {0}};
    rmt_new_rx_channel(&rx_cfg, &g_rx_chan);
    // 4. Register RX done callback
    rmt_rx_event_callbacks_t cbs = {.on_recv_done = rmt_rx_done_cb};
    rmt_rx_register_event_callbacks(g_rx_chan, &cbs, NULL);
    // 5. Allocate DMA-capable buffer for RMT symbols
    g_rx_buf_sz = rx_cfg.mem_block_symbols * sizeof(rmt_symbol_word_t);
    g_rx_buf = heap_caps_malloc(g_rx_buf_sz, MALLOC_CAP_DMA);
    // 6. Configure RMT receive parameters
    g_rx_cfg.signal_range_min_ns = 1000;    // Filter out pulses < 1 us
    g_rx_cfg.signal_range_max_ns = 2000000; // Max pulse 2 ms
    g_rx_cfg.flags.en_partial_rx = 0;
    // 7. Enable and arm RMT
    rmt_enable(g_rx_chan);
    rmt_receive(g_rx_chan, g_rx_buf, g_rx_buf_sz, &g_rx_cfg);
    // 8. Create event queue and processing task
    s_rmt_evt_q = xQueueCreate(10, sizeof(rmt_rx_done_event_data_t));
    xTaskCreatePinnedToCore(monitor_task, "monitor_task", 4096, NULL, 5, NULL, 0);
}
