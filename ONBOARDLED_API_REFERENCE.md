# Onboard LED API Reference Card

## Quick API Summary

### Core Functions
```c
// Initialization
onboardled_begin(pin, active_low);

// Basic Control (NULL = semantic default)  
onboardled_on(color);                    // NULL = white
onboardled_off();
onboardled_toggle(color);                // NULL = white

// Instant Patterns (blocking)
onboardled_success(color);               // NULL = green, 3 blinks
onboardled_failure(color);               // NULL = red, 2 blinks  
onboardled_quick_flash(count, color);    // NULL = blue, quick flashes

// Background Patterns (non-blocking, returns bool)
onboardled_start_success(color);         // NULL = green
onboardled_start_failure(color);         // NULL = red
onboardled_start_blink(on, off, count, color); // NULL = blue
onboardled_stop_pattern();
```

### Color System
```c
// Pointer-based design
NULL                     // Use semantic default color
&ONBOARDLED_COLOR_RED    // Use specific color
&my_custom_color         // Use custom color

// Predefined colors
ONBOARDLED_COLOR_RED     // {255, 0, 0}   - Errors
ONBOARDLED_COLOR_GREEN   // {0, 255, 0}   - Success  
ONBOARDLED_COLOR_BLUE    // {0, 0, 255}   - Neutral
ONBOARDLED_COLOR_WHITE   // {255, 255, 255} - Basic
ONBOARDLED_COLOR_ORANGE  // {255, 165, 0} - Activity
ONBOARDLED_COLOR_YELLOW  // {255, 255, 0} - Warnings
```

### Semantic Defaults (when color = NULL)
| Function Type | Default Color | Use Case |
|---------------|---------------|----------|
| Basic (`on`, `toggle`) | White | General operations |
| Activity (`blink`, `flash`) | Blue | Status indication |
| Success (`success`) | Green | Positive feedback |
| Error (`failure`, `sos`) | Red | Error indication |
| Busy (`busy`) | Orange | Activity indication |

### Pattern Types
| Pattern | Blocking | Non-Blocking | Description |
|---------|----------|--------------|-------------|
| `success` | ✅ | ✅ | 3 green blinks |
| `failure` | ✅ | ✅ | 2 red blinks |
| `quick_flash` | ✅ | ✅ | Fast blue flashes |
| `blink` | ✅ | ✅ | Custom timing |
| `heartbeat` | ✅ | ✅ | Lub-dub pattern |
| `sos` | ✅ | ✅ | Morse code SOS |

### Common Usage Patterns
```c
// Startup sequence
onboardled_begin(8, false);
onboardled_quick_flash(3, NULL);              // Blue startup
onboardled_success(NULL);                     // Green ready

// Error handling  
if (error) {
    onboardled_stop_pattern();
    onboardled_start_failure(NULL);           // Red error
}

// Custom colors
onboardled_color_t purple = {128, 0, 128};
onboardled_success(&purple);                  // Purple success

// Background activity
onboardled_start_blink(500, 500, 0, NULL);   // Infinite blue blink
```

### Backward Compatibility
```c
blink_init();           // → onboardled_begin(CONFIG_BLINK_GPIO, false)
blink_toggle();         // → onboardled_toggle(NULL)  
blink_get_period_ms();  // Still works
blink_set_period_ms();  // Still works
```

## Full Function List

### Basic Control
- `onboardled_begin(pin, active_low)`
- `onboardled_on(color)` 
- `onboardled_off()`
- `onboardled_toggle(color)`
- `onboardled_set_active_low(active_low)`

### Blocking Patterns  
- `onboardled_flash(on_ms, color)`
- `onboardled_quick_flash(count, color)`
- `onboardled_success(color)`
- `onboardled_failure(color)`
- `onboardled_sos(unit_ms, color)`
- `onboardled_heartbeat(repeats, short_ms, long_ms, gap_ms, color)`
- `onboardled_steady_blink(delay_ms, count, color)`
- `onboardled_dynamic_blink(on_ms, off_ms, count, color)`
- `onboardled_signal_code(count, on_ms, off_ms, gap_ms, color)`
- `onboardled_error_code(code, repeat, on_ms, off_ms, group_gap_ms, color)`
- `onboardled_burst(groups, per_group, on_ms, off_ms, gap_ms, color)`
- `onboardled_busy(cycles, on_ms, off_ms, color)`
- `onboardled_on_for(ms, color)`
- `onboardled_off_for(ms)`

### Non-Blocking Patterns (return bool)
- `onboardled_start_blink(on_ms, off_ms, count, color)`
- `onboardled_start_success(color)`  
- `onboardled_start_failure(color)`
- `onboardled_start_sos(unit_ms, color)`
- `onboardled_start_heartbeat(repeats, short_ms, long_ms, gap_ms, color)`
- `onboardled_start_quick_flash(count, color)`

### Pattern Management
- `onboardled_stop_pattern()`
- `onboardled_is_pattern_running()` → bool
- `onboardled_get_current_pattern()` → uint32_t

### Utility
- `onboardled_test_pattern()` 
- `onboardled_get_period_ms()` → uint32_t
- `onboardled_set_period_ms(period)`

---
**Documentation**: See `ONBOARDLED_README.md` for full details  
**Quick Start**: See `ONBOARDLED_QUICKSTART.md` for examples  
**Build**: `idf.py build && idf.py -p COM9 flash monitor`