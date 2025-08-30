
/**
 * @file blink.h
 * @brief Public API for LED blinking module.
 *
 * Provides initialization and toggling functions for LED control.
 */

#ifndef BLINK_H
#define BLINK_H

/**
 * @brief Initialize the LED (strip or GPIO) for blinking.
 */
void blink_init(void);

/**
 * @brief Toggle the LED state (on/off).
 */
void blink_toggle(void);

#include "blink_config.h"
#endif // BLINK_H
