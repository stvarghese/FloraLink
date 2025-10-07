
#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <esp_err.h>
#include <stdbool.h>

#define WIFI_SSID_MAX_LEN 64

#ifndef WIFI_RESET_PIN
#define WIFI_RESET_PIN 9
#endif

// Attempt to connect using NVM credentials, fallback to AP if needed
esp_err_t wifi_setup(void);

// Get the current WiFi SSID (station mode)
const char *wifi_get_ssid(void);

// Check if WiFi is currently connected
bool wifi_is_connected(void);

// Callback for GPIO button to reset WiFi credentials
// (to be used with gpiobutton_init)
void wifi_reset_button_cb(uint32_t gpio_num, int64_t *timestamp_us);

// Configure WiFi for active mode (responsive settings)
void wifi_configure_active_mode(void);

// Configure WiFi for sleep mode (power-saving settings)
void wifi_configure_sleep_mode(void);

#endif // WIFI_SETUP_H
