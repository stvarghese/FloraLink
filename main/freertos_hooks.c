/*
 * Minimal FreeRTOS hook implementations required when the SDK config
 * enables application hooks (CONFIG_FREERTOS_USE_IDLE_HOOK=y).
 *
 * Provide an empty vApplicationIdleHook() so the linker has a definition.
 */

#include "freertos/FreeRTOS.h"

/*
 * Called by the RTOS idle task if CONFIG_FREERTOS_USE_IDLE_HOOK is set.
 * Keep this implementation tiny and non-blocking. If you need to do
 * application work in idle, ensure it's safe and yields appropriately.
 */
void vApplicationIdleHook(void)
{
    /* Intentionally empty: application-specific low-priority background
       work can be added here if desired. Keep fast and non-blocking. */
}
