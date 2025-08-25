
/**
 * @file blink.c
 * @brief LED control module for blinking functionality.
 *
 * This module provides initialization and toggling functions for both addressable LED strips
 * and simple GPIO LEDs, depending on project configuration. It abstracts the hardware details
 * so the application can blink an LED with a simple API.
 */

#include "blink.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "esp_log.h"

#ifdef CONFIG_BLINK_LED_STRIP
// Handle for addressable LED strip
static led_strip_handle_t led_strip;
// State variable for LED (on/off)
static uint8_t s_led_state = 0;

/**
 * @brief Initialize the addressable LED strip for blinking.
 *
 * Configures the LED strip using the selected backend (RMT or SPI) and ensures all LEDs are off.
 */
void blink_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_BLINK_GPIO,
        .max_leds = 1,
    };
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    led_strip_new_spi_device(&strip_config, &spi_config, &led_strip);
#else
#error "unsupported LED strip backend"
#endif
    led_strip_clear(led_strip);
}

/**
 * @brief Toggle the LED state (on/off) for the addressable LED strip.
 *
 * Turns the LED on or off by setting the pixel color and refreshing the strip.
 */
void blink_toggle(void)
{
    s_led_state = !s_led_state;
    if (s_led_state)
    {
        led_strip_set_pixel(led_strip, 0, 0, 0, 1); // Set pixel to blue (example)
        led_strip_refresh(led_strip);
    }
    else
    {
        led_strip_clear(led_strip);
    }
}

#else // GPIO LED
// State variable for GPIO LED (on/off)
static uint8_t s_led_state = 0;

/**
 * @brief Initialize the GPIO pin for blinking.
 *
 * Configures the selected GPIO as output and ensures it is off.
 */
void blink_init(void)
{
    gpio_reset_pin(CONFIG_BLINK_GPIO);
    gpio_set_direction(CONFIG_BLINK_GPIO, GPIO_MODE_OUTPUT);
}

/**
 * @brief Toggle the GPIO LED state (on/off).
 *
 * Flips the state and sets the GPIO level accordingly.
 */
void blink_toggle(void)
{
    s_led_state = !s_led_state;
    gpio_set_level(CONFIG_BLINK_GPIO, s_led_state);
}
#endif
