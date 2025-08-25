/**
 * @file hcsr04-example.c.c
 * @author Aad van Gerwen
 * @brief Example of usage hcsr04 driver
 * @version 0.1
 * @date 2025-03-5
 **/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hcsr04_driver.h"



void hcsr04_task(void *pvParameters)
{
    esp_err_t return_value = ESP_OK;
    (void) UltrasonicInit();

    
    // create variable which stores the measured distance
    static uint32_t afstand = 0;

    while (1) {
        return_value = UltrasonicMeasure(100, &afstand);
        UltrasonicAssert(return_value);
        if (return_value == ESP_OK) {
            printf ("Afstand: %ld\n", afstand);
        }    
    
        // 0,5 second delay before starting new measurement
        vTaskDelay(500 / portTICK_PERIOD_MS);
    } 
}

void app_main(void)
{
    // Create measurement task
    xTaskCreate(hcsr04_task, "HSRC04 task", 2048, NULL, 4, NULL);
}
