#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"

// Include heap tracing configuration
#include "heap_tracing.h"

// Heap tracing helpers (only active when HEAP_TRACING_ENABLED is 1)
#if HEAP_TRACING_ENABLED

typedef struct
{
    size_t heap_before;
    const char *tag;
    const char *function;
} heap_trace_context_t;

// Start heap tracing for a function
heap_trace_context_t heap_trace_start(const char *tag, const char *function);

// End heap tracing and check for significant changes
void heap_trace_end(heap_trace_context_t *ctx, size_t leak_threshold);

// Convenience macros for easy use
#define HEAP_TRACE_START(tag) heap_trace_context_t _heap_ctx = heap_trace_start(tag, __FUNCTION__)
#define HEAP_TRACE_END(threshold) heap_trace_end(&_heap_ctx, threshold)
#define HEAP_TRACE_END_DEFAULT() heap_trace_end(&_heap_ctx, DEFAULT_HEAP_LEAK_THRESHOLD)
#define HEAP_TRACE_END_TIMER() heap_trace_end(&_heap_ctx, TIMER_HEAP_LEAK_THRESHOLD)

#else
// When disabled, these become no-ops
typedef struct
{
    int dummy;
} heap_trace_context_t;
#define HEAP_TRACE_START(tag) \
    do                        \
    {                         \
        (void)(tag);          \
    } while (0)
#define HEAP_TRACE_END(threshold) \
    do                            \
    {                             \
        (void)(threshold);        \
    } while (0)
#define HEAP_TRACE_END_DEFAULT() \
    do                           \
    {                            \
    } while (0)
#define HEAP_TRACE_END_TIMER() \
    do                         \
    {                          \
    } while (0)
#endif

// Anonymize a string from position 'pos' for 'len' characters (replace with '*')
void anonymize_string(char *str, uint8_t pos, size_t len);

// Trim whitespace from both ends of a string (in-place)
void str_trim(char *str);

// Print a buffer as a hex dump (for debugging)
void hex_dump(const void *buf, size_t len);

// Safe strncpy that always null-terminates
void safe_strncpy(char *dst, const char *src, size_t dst_size);

// Format a MAC address as a string (XX:XX:XX:XX:XX:XX)
void format_mac(const uint8_t mac[6], char *out_str, size_t out_str_len);

// Convert string to int with error checking. Returns 0 on success, -1 on error.
int str_to_int(const char *str, int *out);

// Convert int to string. Returns pointer to buf, or NULL on error.
char *int_to_str(int value, char *buf, size_t bufsize);

// URL encode/decode helpers
int url_encode(const char *src, char *dst, size_t dst_size);
int url_decode(const char *src, char *dst, size_t dst_size);

#endif // COMMONUTILS_H
