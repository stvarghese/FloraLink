/**
 * @file heap_tracing.h
 * @brief Heap tracing configuration
 *
 * This file configures heap tracing behavior at compile time.
 * It is automatically included by commonutils.h.
 */

#ifndef HEAP_TRACING_H
#define HEAP_TRACING_H

// Enable/disable heap tracing instrumentation globally
// Set to 0 to disable all heap tracing (saves memory and performance)
// Set to 1 to enable heap tracing (default)
#define HEAP_TRACING_ENABLED 0

// Default threshold for heap leak warnings (bytes)
// Allocations smaller than this won't trigger warnings
#define DEFAULT_HEAP_LEAK_THRESHOLD 50

// Threshold for timer-related functions (bytes)
// Timer allocations are expected to be larger
#define TIMER_HEAP_LEAK_THRESHOLD 2000

#endif // HEAP_TRACING_H