idf_component_register(
    SRCS
        #"freertos_hooks.c"
        "appl_nvm.c"
        "nodeio_services.c"
        "nodeio_sensors.c"
        "onboardled.c"
        "nodeioprotocol.c"
        "nvm.c"
        "websockserver.c"
        "webserver.c"
        "nodeio.c"
        "misc.c"
        "tasks.c"
        "monitor.c"
        "distance.c"
        "wifi_setup.c"
        "modemanager.c"
        "dns_server.c"
        "gpiobutton.c"
        "commonutils.c"
    #idf_component_register(SRCS "test_led_app.c" "onboardled.c" "freertos_hooks.c"
    INCLUDE_DIRS "."
    # REQUIRES: list each component on its own line so you can
    # easily comment out unwanted modules. Example:
    #    # esp_http_server
    REQUIRES
        esp_wifi
        esp_timer
        esp_http_server
        esp_event
        nvs_flash
        driver
        json
    EMBED_FILES
        webpage/main.css
        webpage/main.js)
