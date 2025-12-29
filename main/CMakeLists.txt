if(NOT CONFIG_TEST_APP_MODE_SECBOOT AND NOT CONFIG_TEST_APP_MODE_LED)
    # Normal app mode - register the full FloraLink application
    idf_component_register(
        SRCS
            #"freertos_hooks.c"
            "appl_nvm.c"
            "nodeio_services.c"
            "nodeio_sensors.c"
            "onboardled.c"
            "nodeioprotocol.c"
            "nvm.c"
            "websockserver.c"  # TODO: Update for ESP-IDF 5.5 WebSocket API
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
            "secure_boot_status.c"
        #idf_component_register(SRCS "test_led_app.c" "onboardled.c" "freertos_hooks.c"
        INCLUDE_DIRS "."
        # REQUIRES: list each component on its own line so you can
        # easily comment out unwanted modules. Example:
        #   esp_http_server
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
elseif(CONFIG_TEST_APP_MODE_SECBOOT)
    # Secure boot test mode
    idf_component_register(
        SRCS
            "freertos_hooks.c"
            "secure_boot_status.c"
            "../components/test_apps/secboot/test_secboot_app.c"
        INCLUDE_DIRS
            "."
            "../components/test_apps/secboot"
        REQUIRES
            nvs_flash
            driver
    )
elseif(CONFIG_TEST_APP_MODE_LED)
    # LED test mode
    idf_component_register(
        SRCS
            "freertos_hooks.c"
            "onboardled.c"
            "../components/test_apps/led/test_led_app.c"
        INCLUDE_DIRS
            "."
            "../components/test_apps/led"
        REQUIRES
            driver
    )
endif()
