#include "modemanager.h"
#include <esp_sleep.h>
#include <esp_log.h>

static const char *TAG = "ModeManager";

void modemanager_light_sleep(void)
{
    ESP_LOGI(TAG, "Entering light sleep mode");
    esp_light_sleep_start();
}

void modemanager_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep mode");
    esp_deep_sleep_start();
}
