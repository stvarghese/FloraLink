// blink_config.h
#ifndef BLINK_CONFIG_H
#define BLINK_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Get the current blink period in ms
    uint32_t blink_get_period_ms(void);
    // Set the blink period in ms (range-limited inside implementation)
    void blink_set_period_ms(uint32_t period_ms);

// Expose min/max blink period for use in UI and backend
#define BLINK_PERIOD_MIN 100
#define BLINK_PERIOD_MAX 10000

#ifdef __cplusplus
}
#endif

#endif // BLINK_CONFIG_H
