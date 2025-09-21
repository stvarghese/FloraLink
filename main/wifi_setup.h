
#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <esp_err.h>

#define WIFI_SSID_MAX_LEN 64

// Attempt to connect using NVM credentials, fallback to AP if needed
esp_err_t wifi_setup(void);
// Get the current WiFi SSID (station mode)

const char *wifi_get_ssid(void);

// Monitor FLASH button and reset WiFi credentials if pressed
void wifi_monitor_reset(void);

#endif // WIFI_SETUP_H
