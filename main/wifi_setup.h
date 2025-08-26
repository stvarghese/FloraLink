#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <esp_err.h>

// Initialize Wi-Fi in station mode and connect to the configured SSID
esp_err_t wifi_setup(void);

#endif // WIFI_SETUP_H
