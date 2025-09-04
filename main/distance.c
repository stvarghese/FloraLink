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
#include "webserver.h"
#include "hcsr04_driver.h"
#include "esp_log.h"

static const char *TAG = "Ultrasonic";

void distance_publish(pub_t publisher, uint32_t distance)
{
    if (publisher == PUB_LOG)
    {
        ESP_LOGI(TAG, "Measured distance: %d cm", distance);
    }
    else if (publisher == PUB_WEBSERVER)
    {
        webserver_publish_distance(distance);
    }
}

void distance_publish_err(pub_t publisher, esp_err_t result)
{
    if (publisher == PUB_LOG)
    {
        ESP_LOGE(TAG, "Failed to measure distance: %s, code: 0x%X", esp_err_to_name(result), result);
    }
    else if (publisher == PUB_WEBSERVER)
    {
        webserver_publish_error((int32_t)result);
    }
}

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
