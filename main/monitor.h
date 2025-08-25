
/**
 * @file monitor.h
 * @brief Public API for RMT monitoring module.
 *
 * Provides initialization for the RMT monitor, which logs digital pulse timings on a GPIO.
 */

#ifndef MONITOR_H
#define MONITOR_H

/**
 * @brief Initialize the RMT monitor module.
 *
 * Configures the monitored GPIO, RMT RX channel, event queue, and processing task.
 * Call this once during system initialization.
 */
void monitor_init(void);

#endif // MONITOR_H
