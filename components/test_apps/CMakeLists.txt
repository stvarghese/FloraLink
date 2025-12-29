# Test Applications Component
# This component dispatcher manages which test app (if any) gets built.
# Each test app is a self-contained module with its own sources and dependencies.
#
# Can be controlled via:
#   1. Kconfig: Component config → Test Applications → Select Test Application Mode
#   2. CMake -D flag: idf.py -D CONFIG_TEST_APP_MODE_SECBOOT=y build

# Check if -D flag was used to override config
if(CONFIG_TEST_APP_MODE_SECBOOT)
    # Secure Boot Test App
    idf_component_register(
        SRCS
            "secboot/test_secboot_app.c"
            "../main/secure_boot_status.c"
        INCLUDE_DIRS
            "secboot"
            "../main"
        REQUIRES
            nvs_flash
            driver
    )

elseif(CONFIG_TEST_APP_MODE_LED)
    # LED Pattern Test App
    idf_component_register(
        SRCS
            "led/test_led_app.c"
        INCLUDE_DIRS
            "led"
        REQUIRES
            driver
            onboardled
    )

else()
    # No test app selected - register empty component
    idf_component_register()

endif()
