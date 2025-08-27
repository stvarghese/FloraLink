#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <esp_err.h>

#define WIFI_SSID_MAX_LEN 64
// Initialize Wi-Fi in station mode and connect to the configured SSID
esp_err_t wifi_setup(void);

// Get the current WiFi SSID (station mode)
const char *wifi_get_ssid(void);

#endif // WIFI_SETUP_H
