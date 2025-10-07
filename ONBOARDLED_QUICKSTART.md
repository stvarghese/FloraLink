# Onboard LED Quick Start Guide

## 5-Minute Setup

### 1. Initialize
```c
#include "onboardled.h"

// In your app_main() or init function:
onboardled_begin(8, false);  // GPIO 8, active high
```

### 2. Basic Usage
```c
// Turn on with default white
onboardled_on(NULL);

// Turn on with custom color
onboardled_on(&ONBOARDLED_COLOR_GREEN);

// Toggle with default white
onboardled_toggle(NULL);

// Turn off
onboardled_off();
```

### 3. Quick Patterns
```c
// Success indication (green, 3 blinks)
onboardled_success(NULL);

// Error indication (red, 2 blinks)  
onboardled_failure(NULL);

// Quick activity flash (blue, 3 flashes)
onboardled_quick_flash(3, NULL);
```

### 4. Non-Blocking for Responsive Systems
```c
// Start heartbeat in background (blue)
if (onboardled_start_heartbeat(5, 100, 300, 1000, NULL)) {
    ESP_LOGI("APP", "Heartbeat started, system stays responsive");
}

// Stop any running pattern
onboardled_stop_pattern();

// Check if pattern is running
if (onboardled_is_pattern_running()) {
    ESP_LOGI("APP", "Pattern is active");
}
```

## Common Patterns

### System Status
```c
// Startup sequence
onboardled_quick_flash(3, NULL);                    // Blue startup
onboardled_success(NULL);                           // Green "ready"

// Error handling
if (error_condition) {
    onboardled_stop_pattern();                      // Stop current
    onboardled_start_failure(NULL);                 // Red error
}

// WiFi status  
onboardled_color_t wifi_blue = {0, 100, 255};
onboardled_start_blink(500, 500, 0, &wifi_blue);   // Connecting (infinite)
// ... when connected ...
onboardled_stop_pattern();
onboardled_success(NULL);                           // Connected
```

### Custom Colors
```c
// Define custom colors
onboardled_color_t purple = {128, 0, 128};
onboardled_color_t orange = {255, 165, 0}; 

// Use in patterns
onboardled_flash(1000, &purple);                   // Purple flash
onboardled_start_blink(200, 200, 10, &orange);    // Orange activity
```

## Migration from Old Blink API

### Automatic Compatibility
Your existing code continues to work unchanged:
```c
blink_init();           // Still works
blink_toggle();         // Still works  
blink_get_period_ms();  // Still works
```

### Enhanced Alternatives
```c
// OLD: blink_toggle()
// NEW: onboardled_toggle(NULL)               // Same effect + RGB support

// OLD: No color support
// NEW: onboardled_toggle(&ONBOARDLED_COLOR_RED)  // Now possible!
```

## RGB LED vs GPIO LED

The module automatically adapts:

- **RGB LED Strip**: Full color support, precise color control
- **GPIO LED**: Ignores color parameters, maintains on/off functionality
- **Configuration**: Set via `CONFIG_BLINK_LED_STRIP` in menuconfig

## Key Concepts

### NULL = Semantic Default
```c
onboardled_success(NULL);       // Green (semantic for success)
onboardled_failure(NULL);       // Red (semantic for failure)  
onboardled_flash(500, NULL);    // Blue (semantic for activity)
```

### Pointer = Custom Color
```c
onboardled_color_t custom = {255, 128, 0};  // Orange
onboardled_success(&custom);                // Orange success pattern
```

### Blocking vs Non-Blocking
```c
// BLOCKING: Good for simple apps, test sequences
onboardled_success(NULL);                   // Blocks for ~720ms

// NON-BLOCKING: Good for web servers, real-time apps  
onboardled_start_success(NULL);             // Returns immediately
```

## Troubleshooting

### LED Not Working
1. Check GPIO pin number: `onboardled_begin(CORRECT_PIN, false)`
2. Check polarity: Try `onboardled_begin(pin, true)` for active-low LEDs
3. Verify power: Ensure LED/strip has adequate power supply

### Colors Not Working
1. Confirm RGB LED strip configuration in menuconfig
2. Check `CONFIG_BLINK_LED_STRIP=y`
3. GPIO LEDs ignore colors (normal behavior)

### Patterns Not Starting
1. Check return value: `if (onboardled_start_blink(...)) { /* success */ }`
2. Stop existing pattern first: `onboardled_stop_pattern()`
3. Only one pattern can run at a time

## Next Steps

- Read the full [ONBOARDLED_README.md](ONBOARDLED_README.md) for comprehensive API reference
- Explore advanced patterns: `heartbeat`, `sos`, `signal_code`
- Customize colors for your application's visual language
- Integrate non-blocking patterns with your system architecture

## Build and Test

```powershell
idf.py build
idf.py -p COM9 flash monitor

# Look for test pattern in logs:
# "Starting onboard LED RGB test pattern"
```

Happy coding! 🎉