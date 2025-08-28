
/**
 * @file monitor.h
 * @brief Public API for RMT monitoring module.
 *
 * Provides initialization for the RMT monitor, which logs digital pulse timings on a GPIO.
 */

#ifndef MONITOR_H
#define MONITOR_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
	size_t free_heap;
	size_t min_free_heap;
	uint64_t uptime_ms;
	float cpu_load; // 0.0 to 1.0 (fraction of time not spent in idle)
} device_stats_t;

void monitor_get_device_stats(device_stats_t *stats);

// RMT event processing function (to be called from a task)
void monitor_process_rmt_rx(void);

/**
 * @brief Initialize the RMT monitor module.
 *
 * Configures the monitored GPIO, RMT RX channel, event queue, and processing task.
 * Call this once during system initialization.
 */
void monitor_init(void);

void monitor_update_cpu_load(void);

void vApplicationIdleHook(void);

#endif // MONITOR_H
