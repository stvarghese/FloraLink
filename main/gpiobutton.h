#ifndef GPIOBUTTON_H
#define GPIOBUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include "driver/gpio.h"

extern QueueHandle_t gpiobutton_evt_queue;

// Button event callback type
typedef void (*gpiobutton_callback_t)(uint32_t gpio_num, int64_t *timestamp_us);

typedef struct
{
    gpio_num_t gpio_num;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpiobutton_config_t;

// User must define this table in a .c file
extern const gpiobutton_config_t gpiobutton_config_table[];
extern const int gpiobutton_config_table_size;

// Initialize a GPIO button with interrupt and callback
void gpiobutton_init(gpio_num_t gpio_num, gpiobutton_callback_t cb);

// Multi-press detection for a button (call on each event, pass timestamp)
bool gpiobutton_detect_multi_press_event(gpio_num_t gpio_num, int64_t event_time_us, int required_presses, int timewindow_ms);

// Multi-press detection for a button (polling method)
bool gpiobutton_detect_multi_press_poll(gpio_num_t gpio_num, int required_presses, int timewindow_ms);

// Process button events from the queue, call user callback if set
void process_gpiobutton_events();

#endif // GPIOBUTTON_H
