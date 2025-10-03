/**
 * @file HEAP_TRACING.md
 * @brief Heap Tracing System Documentation
 * 
 * This document explains how to use the heap tracing system for memory leak detection.
 */

# Heap Tracing System

## Overview

The heap tracing system provides automatic memory leak detection by tracking heap usage before and after function calls. It's designed to be lightweight and can be completely disabled at compile time.

## Quick Start

### Basic Usage

```c
#include "commonutils.h"

void my_function(void) {
    HEAP_TRACE_START("MY_TAG");
    
    // Your function code here
    char *buffer = malloc(100);
    // ... do work ...
    free(buffer);
    
    HEAP_TRACE_END_DEFAULT(); // Uses 50-byte threshold
}
```

### Custom Threshold

```c
void my_function_with_timers(void) {
    HEAP_TRACE_START("TIMER_TAG");
    
    // Code that creates timers (expected allocation)
    esp_timer_create(&args, &timer);
    
    HEAP_TRACE_END(1000); // Higher threshold for timer allocations
}
```

## Configuration

### Compile-Time Control

**Enable/Disable Globally:**
```c
// In your build system or project header:
#define HEAP_TRACING_ENABLED 0  // Disable all heap tracing
// or
#define HEAP_TRACING_ENABLED 1  // Enable heap tracing (default)
```

**Using heap_tracing.h:**

The configuration is automatically included when you use commonutils.h:
```c
#include "commonutils.h"  // Automatically includes heap_tracing.h
```

You can override settings by editing `main/heap_tracing.h` or by defining macros before including:

### Runtime Behavior

When `HEAP_TRACING_ENABLED = 1`:
- Functions trace heap usage before and after execution
- Memory leaks above threshold trigger warnings
- Small allocations are logged at DEBUG level

When `HEAP_TRACING_ENABLED = 0`:
- All macros become no-ops
- Zero runtime overhead
- No memory usage for tracing

## API Reference

### Macros

#### `HEAP_TRACE_START(tag)`
- **Purpose**: Start heap monitoring for a function
- **Parameters**: 
  - `tag`: String identifier for log messages
- **Usage**: Call at the beginning of function

#### `HEAP_TRACE_END_DEFAULT()`
- **Purpose**: End heap monitoring with 50-byte threshold
- **Usage**: Call before function returns

#### `HEAP_TRACE_END(threshold)`
- **Purpose**: End heap monitoring with custom threshold
- **Parameters**:
  - `threshold`: Minimum bytes to trigger leak warning
- **Usage**: Use for functions with expected allocations

### Advanced API

#### `heap_trace_context_t heap_trace_start(tag, function)`
- **Purpose**: Manual heap tracing start
- **Returns**: Context structure for heap_trace_end()

#### `void heap_trace_end(ctx, threshold)`
- **Purpose**: Manual heap tracing end
- **Parameters**:
  - `ctx`: Context from heap_trace_start()
  - `threshold`: Leak warning threshold

## Log Output Examples

### Normal Operation
```
D (1234) MY_TAG: [MY_TAG] Heap before my_function: 170188
D (1235) MY_TAG: [MY_TAG] Heap after my_function: 170188 (delta: 0)
```

### Memory Leak Detection
```
D (1234) TIMER_TAG: [TIMER_TAG] Heap before create_timer: 170188
D (1235) TIMER_TAG: [TIMER_TAG] Heap after create_timer: 169292 (delta: -896)
D (1235) TIMER_TAG: Small allocation in create_timer: 896 bytes
```

### Large Leak Warning
```
D (1234) LEAK_TAG: [LEAK_TAG] Heap before leaky_function: 170188
D (1235) LEAK_TAG: [LEAK_TAG] Heap after leaky_function: 168188 (delta: -2000)
W (1235) LEAK_TAG: Memory leak detected in leaky_function: 2000 bytes (before: 170188, after: 168188)
```

## Best Practices

### Threshold Selection

- **Default (50 bytes)**: Most functions
- **100+ bytes**: JSON parsing, string operations
- **1000+ bytes**: Timer creation, large buffer allocation
- **Custom**: Based on expected allocation size

### Tag Naming

Use descriptive, consistent tags:
```c
HEAP_TRACE_START("WEBSOCKET");    // WebSocket operations
HEAP_TRACE_START("JSON_PARSE");   // JSON parsing
HEAP_TRACE_START("TIMER_CREATE"); // Timer operations
HEAP_TRACE_START("BUFFER_ALLOC"); // Buffer management
```

### Performance Considerations

- **Debug builds**: Enable tracing for development
- **Release builds**: Disable tracing for production
- **Selective tracing**: Enable only for suspected leak areas

## Integration Examples

### CMakeLists.txt
```cmake
# Debug build - enable heap tracing
target_compile_definitions(${PROJECT_NAME} PRIVATE HEAP_TRACING_ENABLED=1)

# Release build - disable heap tracing  
# target_compile_definitions(${PROJECT_NAME} PRIVATE HEAP_TRACING_ENABLED=0)
```

### Menuconfig Integration
```kconfig
config HEAP_TRACING_ENABLED
    bool "Enable heap tracing"
    default y
    help
      Enable automatic heap leak detection tracing.
      Disable for production builds to save memory and performance.
```

### Conditional Compilation
```c
#ifdef CONFIG_HEAP_TRACING_ENABLED
#define HEAP_TRACING_ENABLED 1
#else
#define HEAP_TRACING_ENABLED 0
#endif
```

## Troubleshooting

### False Positives

**Timer Allocations**: Use higher thresholds
```c
HEAP_TRACE_END(1000); // Timer allocation expected
```

**Garbage Collection**: ESP32 heap manager may delay frees
```c
// Normal for small variations (< 50 bytes)
```

**JSON Processing**: Parser may cache memory
```c
HEAP_TRACE_END(200); // JSON parser overhead
```

### True Leaks

**Missing free()**: Check malloc/free pairs
**cJSON leaks**: Ensure cJSON_Delete() is called
**Timer leaks**: Add esp_timer_delete() calls
**Buffer leaks**: Check all allocation/deallocation paths

## Implementation Files

- `main/commonutils.h` - API definitions and macros
- `main/commonutils.c` - Implementation
- `main/heap_tracing.h` - Configuration options
- `main/websockserver.c` - Usage example
- `main/nodeio.c` - Usage example