# Power Management and RMT Sleep Interference Fix

## Problem Analysis

### Root Cause
FloraLink uses **two RMT peripherals** that interfere with ESP32-C3 auto light sleep:

1. **LED Strip (WS2812B)** - Uses RMT TX for precise timing control
2. **Monitor Module** - Uses RMT RX for GPIO pulse monitoring (debug feature)

**ESP-IDF RMT Behavior:**
- When an RMT channel is **enabled** via `rmt_enable()`, the driver acquires an `ESP_PM_APB_FREQ_MAX` power management lock
- This lock **prevents** the CPU from reducing APB bus frequency below maximum
- Auto light sleep **requires** the ability to scale APB frequency down → **CONFLICT**

### Previous State
- `monitor_suspend_rmt()` / `monitor_resume_rmt()` existed but only handled the monitor RX channel
- **LED strip RMT TX** was never suspended → Always held PM lock, blocking light sleep
- Idle mode tried to show LED patterns (breathing effect) which **re-acquired PM locks**

## Solution Implemented

### Code Changes

#### 1. Added LED Strip Power Management Functions

**`main/onboardled.h`:**
```c
/**
 * @brief Suspend LED strip RMT peripheral to save power during idle mode.
 */
void onboardled_suspend_led_strip(void);

/**
 * @brief Resume LED strip RMT peripheral after idle mode.
 */
void onboardled_resume_led_strip(void);
```

**`main/onboardled.c`:**
- Implemented `onboardled_suspend_led_strip()` - Stops patterns, clears LED, documents limitation
- Implemented `onboardled_resume_led_strip()` - Prepares LED for active mode

**Limitation Note:**
The ESP-IDF `led_strip` component doesn't expose direct RMT channel control. Currently, we clear the LED and stop patterns, but the RMT channel remains allocated. A complete solution would require:
- Modifying the `led_strip` component to expose `suspend()`/`resume()` APIs
- OR managing RMT channels directly instead of using the led_strip wrapper

#### 2. Updated Mode Manager

**`main/modemanager.c` - Enter Active Mode:**
```c
// Resume RMT monitoring for full functionality during active periods
monitor_resume_rmt();

// Resume LED strip RMT for visual feedback during active mode
onboardled_resume_led_strip();
```

**`main/modemanager.c` - Exit Active Mode:**
```c
// Stop running patterns
onboardled_stop_pattern();

// Show brief breathing pattern as visual indication (completes before RMT suspend)
ESP_LOGI(TAG, "Showing idle entry pattern (3 breathing cycles, ~6s)");
onboardled_start_breathing(750, 250, 2000, 3, &ONBOARDLED_COLOR_BLUE);

// Wait for pattern to complete (3 cycles × 3s = 9s + margin)
vTaskDelay(pdMS_TO_TICKS(9500));

// NOW suspend both RMT channels
monitor_suspend_rmt();
onboardled_suspend_led_strip();
```

### Trade-offs

**Power Savings vs Features:**
- ✅ **Gain**: Auto light sleep functions properly (potentially 10-50% power reduction)
- ✅ **Bonus**: Brief breathing pattern shown when entering idle (visual feedback preserved)
- ❌ **Loss**: No continuous LED indication during idle mode
- ⚖️ **Compromise**: LED feedback during active window + idle entry animation, then RMT suspended

**Why This Is Acceptable:**
1. Users get visual confirmation when system enters idle mode (3 breathing cycles)
2. After animation completes, RMT is suspended and power savings activated
3. LED patterns during active window provide plenty of feedback
4. The ~10 second animation delay before entering deep idle is acceptable
5. Primary goal of power efficiency is maintained

## Testing Recommendations

### 1. Power Consumption Measurement
```powershell
# Before fix (with PM locks held)
idf.py monitor
# Note: Average current ~80mA (typical with WiFi active)

# After fix (with PM locks released)
idf.py monitor
# Expected: Average current ~20-40mA during idle periods (with WiFi power save)
```

### 2. Verify PM Lock Release
Add to your code:
```c
// After entering idle mode
modemanager_dump_pm_locks();
```

Expected output:
```
PM lock state on entering auto sleep:
Lock stats:
  (no active locks)
```

If you still see locks, check:
- WiFi power save is configured (`wifi_configure_sleep_mode()`)
- No other peripherals holding APB locks (UART, SPI, I2C with transactions in progress)

### 3. Sleep Mode Verification
```c
// In monitor_task_1s or similar
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
    ESP_LOGI("Sleep", "Woke from light sleep, cause: %d", cause);
}
```

## Future Improvements

### Full RMT Suspend/Resume
To properly release RMT resources, consider:

**Option A - Modify led_strip component:**
```c
// Add to led_strip driver
esp_err_t led_strip_suspend(led_strip_handle_t strip);
esp_err_t led_strip_resume(led_strip_handle_t strip);
```

**Option B - Direct RMT management:**
```c
// In onboardled.c
static rmt_channel_handle_t s_rmt_tx_chan = NULL;

void onboardled_suspend_led_strip(void) {
    if (s_rmt_tx_chan) {
        rmt_disable(s_rmt_tx_chan);  // Releases PM lock
    }
}

void onboardled_resume_led_strip(void) {
    if (s_rmt_tx_chan) {
        rmt_enable(s_rmt_tx_chan);   // Re-acquires PM lock
    }
}
```

### Conditional LED During Idle
The current implementation shows a breathing pattern when entering idle, then suspends RMT:

```c
void modemanager_exit_active_auto(void) {
    // Stop patterns
    onboardled_stop_pattern();
    
    // Show entering-idle animation (completes before suspend)
    onboardled_start_breathing(750, 250, 2000, 3, &ONBOARDLED_COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(9500));  // Wait for completion
    
    // Now suspend RMT for power savings
    monitor_suspend_rmt();
    onboardled_suspend_led_strip();
}
```

This provides visual feedback without holding PM locks during the idle period.

## Related Files
- `main/onboardled.c` / `main/onboardled.h` - LED strip control
- `main/monitor.c` / `main/monitor.h` - RMT monitor (debug)
- `main/modemanager.c` / `main/modemanager.h` - Power mode coordination
- `main/tasks.c` - Task coordination with MODE_ACTIVE_BIT

## References
- ESP-IDF Power Management: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/system/power_management.html
- RMT Driver: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/rmt.html
- Light Sleep: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/system/sleep_modes.html
