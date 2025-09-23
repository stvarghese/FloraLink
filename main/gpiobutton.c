#include <esp_timer.h>
#include "gpiobutton.h"
#include "freertos/queue.h"
#include "esp_log.h"

// **********Config section************
// GPIO button config table
const gpiobutton_config_t gpiobutton_config_table[] = {
    // FLASH/BOOT button for WiFi reset (GPIO_NUM_9 on ESP32-C3)
    {GPIO_NUM_9, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    // The following are commented out to prevent accidental use of other GPIOs for button logic:
    /*
    {GPIO_NUM_0, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_1, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_2, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_3, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_4, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_5, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_6, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_7, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    {GPIO_NUM_8, GPIO_MODE_INPUT, GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_NEGEDGE},
    */
};
const int gpiobutton_config_table_size = sizeof(gpiobutton_config_table) / sizeof(gpiobutton_config_table[0]);

#define TAG "gpiobutton"

typedef struct
{
    uint32_t gpio_num;
    int64_t timestamp_us;
} gpiobutton_event_t;

QueueHandle_t gpiobutton_evt_queue = NULL;
static gpiobutton_callback_t gpiobutton_user_cb = NULL;

static void IRAM_ATTR gpiobutton_isr_handler(void *arg)
{
    gpiobutton_event_t evt;
    evt.gpio_num = (uint32_t)arg;
    evt.timestamp_us = esp_timer_get_time();
    xQueueSendFromISR(gpiobutton_evt_queue, &evt, NULL);
}

// Returns true if the button is pressed (active LOW)
bool gpiobutton_pressed(gpio_num_t gpio_num)
{
    return gpio_get_level(gpio_num) == 0;
}

// Process button events from the queue, call user callback if set
void process_gpiobutton_events()
{
    gpiobutton_event_t evt;
    while (xQueueReceive(gpiobutton_evt_queue, &evt, 0)) // Non-blocking
    {
        ESP_LOGD(TAG, "Event: GPIO %d at %lld us", evt.gpio_num, evt.timestamp_us);
        if (gpiobutton_user_cb)
        {
            gpiobutton_user_cb(evt.gpio_num, &evt.timestamp_us);
        }
    }
}

// Initialize a GPIO button with interrupt and callback
void gpiobutton_init(gpio_num_t gpio_num, gpiobutton_callback_t cb)
{
    // Find config for this gpio_num
    const gpiobutton_config_t *cfg = NULL;
    extern const gpiobutton_config_t gpiobutton_config_table[];
    extern const int gpiobutton_config_table_size;
    for (int i = 0; i < gpiobutton_config_table_size; ++i)
    {
        if (gpiobutton_config_table[i].gpio_num == gpio_num)
        {
            cfg = &gpiobutton_config_table[i];
            break;
        }
    }
    if (!cfg)
    {
        ESP_LOGE("gpiobutton", "No config found for GPIO %d", gpio_num);
        return;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << cfg->gpio_num,
        .mode = cfg->mode,
        .pull_up_en = cfg->pull_up_en,
        .pull_down_en = cfg->pull_down_en,
        .intr_type = cfg->intr_type};
    gpio_config(&io_conf);
    if (!gpiobutton_evt_queue)
    {
        gpiobutton_evt_queue = xQueueCreate(10, sizeof(gpiobutton_event_t));
    }
    gpio_install_isr_service(0);
    gpio_isr_handler_add(cfg->gpio_num, gpiobutton_isr_handler, (void *)cfg->gpio_num);

    // Set user callback for button interrupt events
    if (cb)
    {
        gpiobutton_user_cb = cb;
    }
}

// Multi-press detection for a button (call on each event, pass timestamp)
bool gpiobutton_detect_multi_press_event(gpio_num_t gpio_num, int64_t event_time_us, int required_presses, int timewindow_ms)
{
    static int press_count = 0;
    static int64_t first_press_time = 0;
    int64_t window_us = (int64_t)timewindow_ms * 1000;

    // Each call is a button press event (falling edge)
    if (press_count == 0)
    {
        first_press_time = event_time_us;
    }
    press_count++;

    // Reset if time window expired
    if (press_count > 0 && (event_time_us - first_press_time > window_us))
    {
        press_count = 0;
        first_press_time = 0;
    }

    if (press_count == required_presses && (event_time_us - first_press_time <= window_us))
    {
        press_count = 0;
        first_press_time = 0;
        return true;
    }
    return false;
}

// Multi-press detection for a button (polling method)
bool gpiobutton_detect_multi_press_poll(gpio_num_t gpio_num, int required_presses, int timewindow_ms)
{
    static int press_count = 0;
    static int64_t first_press_time = 0;
    static bool prev_pressed = false;
    int64_t now = esp_timer_get_time();
    bool pressed = gpiobutton_pressed(gpio_num);
    int64_t window_us = (int64_t)timewindow_ms * 1000;

    bool rising_edge = pressed && !prev_pressed;
    prev_pressed = pressed;
    if (rising_edge) // rising edge
    {
        if (press_count == 0)
        {
            first_press_time = now;
            ESP_LOGD(TAG, "Button first press detected");
        }
        press_count++;
    }

    // Reset if time window expired
    if (press_count > 0 && (now - first_press_time > window_us))
    {
        ESP_LOGD(TAG, "Button pressed %d time(s)", press_count);
        press_count = 0;
        first_press_time = 0;
    }

    if (press_count == required_presses && (now - first_press_time <= window_us))
    {
        ESP_LOGD(TAG, "Button multi-press detected");

        press_count = 0;
        first_press_time = 0;
        return true;
    }
    return false;
}
