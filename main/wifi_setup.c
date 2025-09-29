// FloraLink WiFi Setup - Single Implementation

#include "wifi_setup.h"
#include "nvm.h"
#include <string.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/event_groups.h>
#include "lwip/ip4_addr.h"
#include <esp_http_server.h>
#include <esp_timer.h>
#include "driver/gpio.h"
#include "dns_server.h"
#include "commonutils.h"

#define WIFI_MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static char s_current_ssid[WIFI_SSID_MAX_LEN] = {0};
static const char *TAG = "WiFiSetup";
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;
static char ap_ssid[32] = "FloraLinkAP";
static char ap_password[16] = "passcodeflora";

#include "gpiobutton.h"

// Detect N button presses within a time window (ms) -> moved to gpiobutton.c
// static bool detect_multi_press(int required_presses, int timewindow_ms)
// {
//     static int press_count = 0;
//     static int64_t first_press_time = 0;
//     static bool prev_pressed = false;
//     int64_t now = esp_timer_get_time();
//     bool pressed = wifi_reset_button_pressed();
//     int64_t window_us = (int64_t)timewindow_ms * 1000;

//     bool rising_edge = pressed && !prev_pressed;
//     prev_pressed = pressed;
//     if (rising_edge) // rising edge
//     {
//         if (press_count == 0)
//         {
//             first_press_time = now;
//             ESP_LOGD(TAG, "Button first press detected");
//         }
//         press_count++;
//     }

//     // Reset if time window expired
//     if (press_count > 0 && (now - first_press_time > window_us))
//     {
//         ESP_LOGD(TAG, "Button pressed %d time(s)", press_count);
//         press_count = 0;
//         first_press_time = 0;
//     }

//     if (press_count == required_presses && (now - first_press_time <= window_us))
//     {
//         ESP_LOGD(TAG, "Button multi-press detected");

//         press_count = 0;
//         first_press_time = 0;
//         return true;
//     }
//     return false;
// }

void wifi_monitor_reset(void)
{
    ESP_LOGI(TAG, "WiFi reset triggered: Erasing credentials and restarting in AP mode");
    nvm_erase_block(NVM_BLOCK_WIFI_CREDENTIALS);
    vTaskDelay(100); // Debounce
    esp_restart();
}

// Wrapper to match gpiobutton_callback_t signature
void wifi_reset_button_cb(uint32_t gpio_num, int64_t *timestamp_us)
{
    if (gpio_num == WIFI_RESET_PIN && gpiobutton_detect_multi_press_event(gpio_num, *timestamp_us, 2, 3000))
    {
        wifi_monitor_reset();
    }
}

// ===================== Network Stack Initialization Helper =====================
static void wifi_network_stack_init(bool ap_mode)
{
    static bool event_loop_created = false;
    ESP_ERROR_CHECK(esp_netif_init());
    if (!event_loop_created)
    {
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        event_loop_created = true;
    }
    if (ap_mode)
        esp_netif_create_default_wifi_ap();
    else
        esp_netif_create_default_wifi_sta();
}

// ===================== WiFi Event Handler =====================
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < WIFI_MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: %s", ip4addr_ntoa((const ip4_addr_t *)&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ===================== Captive Portal HTTP Handlers =====================
static esp_err_t credentials_post_handler(httpd_req_t *req)
{
    char buf[128] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    char ssid[32] = {0}, password[64] = {0};
    char *ssid_ptr = strstr(buf, "ssid=");
    char *pass_ptr = strstr(buf, "password=");
    if (ssid_ptr)
    {
        ssid_ptr += 5;
        char *end = strchr(ssid_ptr, '&');
        int len = end ? (end - ssid_ptr) : strlen(ssid_ptr);
        strncpy(ssid, ssid_ptr, len);
        ssid[len] = '\0';
    }
    if (pass_ptr)
    {
        pass_ptr += 9;
        char *end = strchr(pass_ptr, '&');
        int len = end ? (end - pass_ptr) : strlen(pass_ptr);
        strncpy(password, pass_ptr, len);
        password[len] = '\0';
    }
    if (strlen(ssid) > 0 && strlen(password) > 0)
    {
        strncpy(nvm_wifi_credentials_ram.ssid, ssid, sizeof(nvm_wifi_credentials_ram.ssid));
        strncpy(nvm_wifi_credentials_ram.password, password, sizeof(nvm_wifi_credentials_ram.password));
        nvm_write_block(NVM_BLOCK_WIFI_CREDENTIALS, NULL);
        httpd_resp_sendstr(req, "Credentials saved. Rebooting...");
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid credentials");
    return ESP_FAIL;
}

static esp_err_t credentials_get_handler(httpd_req_t *req)
{
    const char *form_html =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
        "<title>FloraLink WiFi Setup</title>"
        "<style>"
        "body { background: linear-gradient(135deg, #e0eafc 0%, #cfdef3 100%); min-height: 100vh; margin: 0; font-family: 'Segoe UI', Arial, sans-serif; display: flex; align-items: center; justify-content: center; }"
        ".card { background: #fff; border-radius: 16px; box-shadow: 0 4px 24px rgba(0,0,0,0.08); padding: 2rem 1.5rem; max-width: 350px; width: 100%; }"
        ".card h2 { margin-top: 0; color: #2d6a4f; font-size: 1.6rem; text-align: center; }"
        "form { display: flex; flex-direction: column; gap: 1.2rem; }"
        "input[type='text'], input[type='password'] { padding: 0.8rem; border-radius: 8px; border: 1px solid #b7e4c7; font-size: 1rem; }"
        "input[type='submit'] { background: #2d6a4f; color: #fff; border: none; border-radius: 8px; padding: 0.8rem; font-size: 1.1rem; cursor: pointer; transition: background 0.2s; }"
        "input[type='submit']:hover { background: #40916c; }"
        "label { font-weight: 500; color: #40916c; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class='card'>"
        "<h2>FloraLink WiFi Setup</h2>"
        "<form method='POST' action='/credentials'>"
        "<label for='ssid'>WiFi SSID</label>"
        "<input id='ssid' name='ssid' type='text' maxlength='32' required autofocus>"
        "<label for='password'>WiFi Password</label>"
        "<input id='password' name='password' type='password' maxlength='64' required>"
        "<input type='submit' value='Connect'>"
        "</form>"
        "</div>"
        "</body>"
        "</html>";
    httpd_resp_sendstr(req, form_html);
    return ESP_OK;
}

// ===================== /connecttest.txt Handler =====================
static esp_err_t connecttest_handler(httpd_req_t *req)
{
    // Windows expects HTTP 200 and a short body ("Microsoft Connect Test")
    const char *body = "Microsoft Connect Test";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ===================== AP Mode Setup =====================
static esp_err_t wifi_setup_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode for WiFi credential entry");
    wifi_network_stack_init(true);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strncpy((char *)ap_config.ap.password, ap_password, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Start DNS hijack server
    dns_server_start("192.168.4.1");

    httpd_handle_t server = NULL;
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.max_uri_handlers = 4;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));
    httpd_uri_t cred_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = credentials_get_handler,
        .user_ctx = NULL};
    httpd_uri_t cred_post = {
        .uri = "/credentials",
        .method = HTTP_POST,
        .handler = credentials_post_handler,
        .user_ctx = NULL};
    // Captive portal detection handlers
    httpd_uri_t captive_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = credentials_get_handler,
        .user_ctx = NULL};
    httpd_uri_t captive_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = credentials_get_handler,
        .user_ctx = NULL};
    httpd_uri_t captive_portal = {
        .uri = "/captiveportal.html",
        .method = HTTP_GET,
        .handler = credentials_get_handler,
        .user_ctx = NULL};
    // Register /connecttest.txt handler for Windows captive portal detection
    static const httpd_uri_t connecttest_uri = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = connecttest_handler,
        .user_ctx = NULL};
    httpd_register_uri_handler(server, &cred_get);
    httpd_register_uri_handler(server, &cred_post);
    httpd_register_uri_handler(server, &captive_204);
    httpd_register_uri_handler(server, &captive_hotspot);
    httpd_register_uri_handler(server, &captive_portal);
    httpd_register_uri_handler(server, &connecttest_uri);
    ESP_LOGI(TAG, "AP started. Connect to SSID '%s', password '%s', then browse to http://192.168.4.1/", ap_ssid, ap_password);
    while (1)
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    return ESP_OK;
}

// ===================== Main WiFi Setup Logic =====================
esp_err_t wifi_setup(void)
{
    bool nvm_ok = nvm_read_block(NVM_BLOCK_WIFI_CREDENTIALS);
    wifi_config_t wifi_config = {0};

    // Wifi reset button init done in init_task()

    if (nvm_ok)
    {
        strncpy((char *)wifi_config.sta.ssid, nvm_wifi_credentials_ram.ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, nvm_wifi_credentials_ram.password, sizeof(wifi_config.sta.password));
        char password_anon[sizeof(wifi_config.sta.password)];
        // strncpy copy password to password_anon for anonymizing
        strncpy(password_anon, (char *)wifi_config.sta.password, sizeof(wifi_config.sta.password));
        anonymize_string(password_anon, 3, strlen(password_anon)); // anonymize from pos 3
        ESP_LOGI(TAG, "Read WiFi credentials from NVM: SSID='%s', Password='%s'",
                 wifi_config.sta.ssid,
                 password_anon);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    else
    {
        // return wifi_setup_ap_mode();
        // Temporarily use default credentials for testing
        const char *default_ssid = "TheDecoRated";
        const char *default_password = "N0freeloading";
        strncpy((char *)wifi_config.sta.ssid, default_ssid, sizeof(wifi_config.sta.ssid));
        strncpy((char *)wifi_config.sta.password, default_password, sizeof(wifi_config.sta.password));
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_LOGW(TAG, "No WiFi credentials in NVM. Using default credentials. Press and hold the WiFi reset button for 3 seconds to enter AP mode.");
    }

    // Copy SSID to global variable for webserver use
    strncpy(s_current_ssid, (char *)wifi_config.sta.ssid, sizeof(s_current_ssid));

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_network_stack_init(false);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to STA SSID:%s", wifi_config.sta.ssid);

        return ESP_OK;
    }
    else
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, fallback to AP mode", wifi_config.sta.ssid);
        return wifi_setup_ap_mode();
    }
}

const char *wifi_get_ssid(void)
{
    return s_current_ssid;
}