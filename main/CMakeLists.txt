idf_component_register(SRCS "misc.c" "tasks.c" "monitor.c" "distance.c" "blink.c" "webserver/webserver.c" "wifi_setup.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_wifi esp_http_server esp_event nvs_flash driver)
