
/**
 * @file misc.c
 * @brief Miscellaneous utility and test functions for the project.
 *
 * This file is intended for code that does not belong to any specific module,
 * such as quick test routines, debug helpers, or general-purpose utilities.
 * Functions here can be used for rapid prototyping or temporary experiments
 * without cluttering the main application logic or feature modules.
 */

#include "misc.h"
#include "esp_log.h"
#include "driver/gpio.h"

/**
 * @brief Example miscellaneous test function.
 *
 * This function demonstrates how to add quick test or utility code to the misc module.
 * It simply logs a message to indicate it was called. Replace or expand this function
 * with your own test routines as needed.
 */

/**
 * @brief Generate a test pulse on GPIO 4 for RMT monitor.
 *
 * This function sets GPIO 4 low for 4us, then high for 10us, then low again.
 * It is used to generate a known pulse for RMT monitoring or level shifter testing.
 *
 * Make sure GPIO 4 is configured as output before calling this function.
 */
void misc_test_function(void)
{
    // Generate a test pulse for RMT monitor (4us low, 10us high)
    gpio_set_level(4, 0);
    esp_rom_delay_us(4);
    gpio_set_level(4, 1);
    esp_rom_delay_us(10);
    gpio_set_level(4, 0);
}
