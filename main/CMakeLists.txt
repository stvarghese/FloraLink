idf_component_register(SRCS "websockserver.c" "_mqttnode.c" "_mqtthub.c" "webserver.c" "nodeio.c" "misc.c" "tasks.c" "monitor.c" "distance.c" "blink.c" "blink_config.c" "webserver.c" "wifi_setup.c"
                      "modemanager.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_wifi esp_timer esp_http_server esp_event nvs_flash driver cJSON)
