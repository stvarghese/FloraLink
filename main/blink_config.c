// blink_config.c
#include "blink_config.h"
#include "sdkconfig.h"

/* Blink period in milliseconds */
static uint32_t s_blink_period_ms = CONFIG_BLINK_PERIOD; // default from menuconfig

/* Get the blink period in milliseconds */
uint32_t blink_get_period_ms(void)
{
    return s_blink_period_ms;
}

/* Set the blink period in milliseconds */
void blink_set_period_ms(uint32_t period_ms)
{
    if (period_ms < BLINK_PERIOD_MIN)
        period_ms = BLINK_PERIOD_MIN;
    if (period_ms > BLINK_PERIOD_MAX)
        period_ms = BLINK_PERIOD_MAX;
    s_blink_period_ms = period_ms;
}
