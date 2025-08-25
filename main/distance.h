

/**
 * @file distance.h
 * @brief Public API for ultrasonic distance measurement module.
 *
 * This module provides a simple interface for initializing and measuring distance
 * using an ultrasonic sensor (such as the HC-SR04) on the ESP32 platform.
 *
 * Configuration:
 * - The sensor's trigger and echo pins should be set in the project configuration or driver.
 * - Ensure the sensor is connected to the correct GPIOs and powered appropriately.
 *
 * Usage Example:
 * @code
 *   #include "distance.h"
 *   void app_main(void) {
 *       distance_init();
 *       uint32_t dist_cm;
 *       if (distance_measure(400, &dist_cm) == ESP_OK) {
 *           printf("Distance: %d cm\n", dist_cm);
 *       }
 *   }
 * @endcode
 */

#ifndef DISTANCE_H
#define DISTANCE_H

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize the ultrasonic sensor hardware.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t distance_init(void);

/**
 * @brief Measure distance using the ultrasonic sensor.
 * @param max_distance Maximum distance to measure (in cm)
 * @param distance_cm Pointer to store the measured distance (in cm)
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t distance_measure(uint32_t max_distance, uint32_t *distance_cm);

#endif // DISTANCE_H
