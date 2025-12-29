# Test Applications Framework

This directory contains modular test applications for FloraLink that can be built independently using CMake -D flags.

## Quick Start

Build and flash a test app:

```bash
# Secure Boot Test
idf.py -D CONFIG_TEST_APP_MODE_SECBOOT=y build
idf.py -p COM_PORT flash monitor

# LED Pattern Test
idf.py -D CONFIG_TEST_APP_MODE_LED=y build
idf.py -p COM_PORT flash monitor

# Normal App (no test mode)
idf.py build
idf.py -p COM_PORT flash monitor
```

## Directory Structure

```
test_apps/
├── secboot/
│   ├── test_secboot_app.c        # Secure boot FIFO testing (8 tests)
│   └── test_secboot_app.h        # Test app header
├── led/
│   ├── test_led_app.c            # LED pattern and button testing
│   └── test_led_app.h            # Test app header
├── CMakeLists.txt                # Dispatcher (not used - see Build System)
├── Kconfig.projbuild             # Config choices (informational)
└── README.md                      # This file
```

## Build System

**Important:** Test apps are NOT built as separate components. Instead, they are compiled as part of the main component when test mode is enabled via `main/CMakeLists.txt`.

### How It Works

When you run:
```bash
idf.py -D CONFIG_TEST_APP_MODE_SECBOOT=y build
```

1. CMake evaluates the `-D` flag and sets `CONFIG_TEST_APP_MODE_SECBOOT=y`
2. `main/CMakeLists.txt` detects this config and:
   - Includes `freertos_hooks.c` (provides `vApplicationIdleHook`)
   - Includes test app source files from `../components/test_apps/`
   - Registers appropriate REQUIRES (nvs_flash, driver, etc.)
3. Main component provides `app_main()` entry point from test app
4. FreeRTOS calls the test app's `app_main()`

## Writing a New Test App

### 1. Create Test App Directory

```bash
mkdir -p components/test_apps/mytest
```

### 2. Write Test Application (`mytest/test_mytest_app.c`)

```c
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "test_mytest";

void app_main(void)
{
    ESP_LOGI(TAG, "My test app started");
    
    // Your test code here
    
    // Optional: delay before restart
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
}
```

### 3. Update `main/CMakeLists.txt`

Add a new `elseif` block to handle your test mode:

```cmake
elseif(CONFIG_TEST_APP_MODE_MYTEST)
    # My Test App
    idf_component_register(
        SRCS
            "freertos_hooks.c"
            # Add your test app sources here
            "../components/test_apps/mytest/test_mytest_app.c"
        INCLUDE_DIRS
            "."
            "../components/test_apps/mytest"
        REQUIRES
            # Add required components here
            driver
    )
```

### 4. Update `components/test_apps/Kconfig.projbuild`

Add your test mode option:

```kconfig
choice TEST_APP_MODE
    prompt "Select Test Application Mode"
    default TEST_APP_MODE_NONE
    
    config TEST_APP_MODE_MYTEST
        bool "My Test App"
        help
            Builds my custom test application.
            
endchoice
```

### 5. Build and Test

```bash
idf.py -D CONFIG_TEST_APP_MODE_MYTEST=y build
idf.py -p COM_PORT flash monitor
```

## Existing Test Apps

### Secure Boot Test (`secboot/test_secboot_app.c`)

**Purpose:** Comprehensive testing of secure boot FIFO functionality

**Tests:**
1. Initialization and magic validation
2. Push single OK entry
3. Push multiple entries (FAIL, OK patterns)
4. Fill FIFO to capacity (10 entries)
5. Overflow behavior (shift-left on full)
6. History string generation
7. Clear boot status
8. Re-initialization after clear

**Run:**
```bash
idf.py -D CONFIG_TEST_APP_MODE_SECBOOT=y build
idf.py -p COM_PORT flash monitor
```

**Expected Output:**
```
I (XXX) test_secboot: Test 1: Initialization and magic validation
I (XXX) test_secboot: [OK] Magic: 0x5EC0B007
...
I (XXX) test_secboot: ======== TEST SUMMARY ========
I (XXX) test_secboot: Total: 8, Passed: 8, Failed: 0
I (XXX) test_secboot: Device will restart in 10 seconds...
```

### LED Pattern Test (`led/test_led_app.c`)

**Purpose:** Test LED pattern generation and GPIO button interrupts

**Features:**
- LED breathing pattern (750-1250ms cycle)
- Quick flash sequences
- Heartbeat patterns
- Disco sequences
- Button interrupt handler (GPIO 9)
- Button controls start/stop of demos

**Run:**
```bash
idf.py -D CONFIG_TEST_APP_MODE_LED=y build
idf.py -p COM_PORT flash monitor
```

**Expected Output:**
```
I (XXX) test_led_app: Starting LED pattern test app
I (XXX) test_led_app: LED breathing test task started, button on GPIO 9
I (XXX) test_led_app: Waiting for button presses (GPIO 9)...
```

Press the BOOT button (GPIO 9) to start LED patterns.

## Architecture Notes

- **MINIMAL_BUILD**: Set to ON in root CMakeLists to keep build lean
- **FreeRTOS Hooks**: `freertos_hooks.c` provides `vApplicationIdleHook` and `vApplicationTickHook`
- **No Separate Component**: Test apps don't run as separate components to avoid MINIMAL_BUILD culling
- **Config-Driven**: Test mode selection via CMake -D flags or menuconfig
- **Integrated Builds**: Test sources compiled as part of main component

## Troubleshooting

**Build Error: "undefined reference to app_main"**
- Ensure test app source file is listed in `SRCS` in `main/CMakeLists.txt`
- Verify `-D CONFIG_TEST_APP_MODE_XXX=y` flag is set

**Build Error: "undefined reference to vApplicationIdleHook"**
- Ensure `freertos_hooks.c` is included in `SRCS`
- Check that `freertos_hooks.c` exists in `main/` directory

**Missing Symbols**
- Check REQUIRES list in `main/CMakeLists.txt` includes needed components
- Example: `driver` for GPIO, `nvs_flash` for NVS access

**Test App Not Running**
- Verify `-D CONFIG_TEST_APP_MODE_XXX=y` was used during build
- Check serial output (115200 baud)
- Ensure device has been flashed: `idf.py -p COM_PORT flash`

## See Also

- `main/CMakeLists.txt` - Test mode configuration
- `components/test_apps/secboot/test_secboot_app.c` - Example: FIFO testing
- `components/test_apps/led/test_led_app.c` - Example: GPIO and pattern testing
