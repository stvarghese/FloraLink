
/**
 * @file distance.c
 * @brief Ultrasonic distance measurement module.
 *
 * This module provides initialization and measurement functions for an ultrasonic sensor
 * (e.g., HC-SR04) using a hardware abstraction driver. It allows the application to
 * easily measure distances in centimeters.
 *
 * Configuration:
 * - The sensor pins and timing are set in the project configuration or driver.
 * - Ensure the hardware is connected as per the driver's requirements.
 */

#include "distance.h"
#include "hcsr04_driver.h"
#include "esp_log.h"

/**
 * @brief Initialize the ultrasonic sensor hardware.
 *
 * Calls the underlying driver to set up the sensor. Must be called before measuring.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t distance_init(void)
{
    return UltrasonicInit();
}

/**
 * @brief Measure distance using the ultrasonic sensor.
 *
 * Triggers the sensor and waits for the echo to calculate distance.
 * @param max_distance Maximum distance to measure (in cm)
 * @param distance_cm Pointer to store the measured distance (in cm)
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t distance_measure(uint32_t max_distance, uint32_t *distance_cm)
{
    return UltrasonicMeasure(max_distance, distance_cm);
}
