idf_component_register(SRCS "nodeioprotocol.c" "nvm.c" "websockserver.c" "webserver.c" "nodeio.c" "misc.c" "tasks.c" "monitor.c" "distance.c" "blink.c" "blink_config.c" "wifi_setup.c"
                      "modemanager.c"  "dns_server.c" "gpiobutton.c" "commonutils.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_wifi esp_timer esp_http_server esp_event nvs_flash driver json
                      EMBED_FILES webpage/main.css webpage/main.js)
