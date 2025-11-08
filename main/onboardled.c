/**
 * @file onboardled.c
 * @brief Consolidated RGB onboard LED control module implementation.
 *
 * This module implements a comprehensive LED control system with RGB color support
 * using an elegant pointer-based API. Functions accept optional color pointers where
 * NULL indicates semantic defaults and valid pointers specify custom colors.
 *
 * Key Implementation Features:
 * - Pointer-based color parameters: NULL = semantic default, pointer = custom
 * - Timer-based state machine for non-blocking patterns
 * - Semantic color system: green=success, red=error, blue=neutral, orange=activity
 * - Hardware abstraction: GPIO LEDs and addressable LED strips
 * - Memory efficient: ~100 bytes pattern context, fallback CONFIG definitions
 *
 * Example usage:
 *
 * // Initialize the LED on GPIO 8, active high
 * onboardled_begin(8, false);
 *
 * // Basic operations with consolidated API
 * onboardled_on(NULL);                           // Default white
 * onboardled_on(&ONBOARDLED_COLOR_GREEN);       // Explicit green
 * onboardled_off();                              // Turn LED off
 * onboardled_toggle(&ONBOARDLED_COLOR_BLUE);    // Toggle with blue
 *
 * // BLOCKING patterns with semantic colors (NULL = semantic default)
 * onboardled_quick_flash(3, NULL);               // Blue quick flashes (blocks ~525ms)
 * onboardled_success(NULL);                      // Green success pattern (blocks ~720ms)
 * onboardled_failure(NULL);                      // Red failure pattern (blocks ~400ms)
 * onboardled_sos(200, NULL);                     // Red SOS pattern (blocks ~3.4s)
 * onboardled_heartbeat(5, 100, 300, 1000, NULL); // Blue heartbeats (blocks ~7s)
 *
 * // Custom colors with consolidated API
 * onboardled_color_t purple = {128, 0, 128};
 * onboardled_success(&purple);                   // Purple success pattern
 * onboardled_flash(500, &ONBOARDLED_COLOR_CYAN); // Cyan flash
 *
 * // NON-BLOCKING patterns with semantic colors (for responsive systems)
 * if (onboardled_start_success(NULL)) {      // Green success pattern
 *     // Success pattern started, system remains responsive
 *     // Continue with other tasks...
 * }
 *
 * // Check if pattern is running
 * if (onboardled_is_pattern_running()) {
 *     ESP_LOGD("APP", "LED pattern active, type: %d", onboardled_get_current_pattern());
 * }
 *
 * // Stop any running pattern (useful for interrupting)
 * onboardled_stop_pattern();
 *
 * // More non-blocking patterns with colors
 * onboardled_start_blink(500, 200, 10, &ONBOARDLED_COLOR_ORANGE); // Orange activity
 * onboardled_start_heartbeat(3, 100, 300, 1000, &ONBOARDLED_COLOR_CYAN); // Cyan heartbeats
 * onboardled_start_sos(150, &ONBOARDLED_COLOR_RED);    // Red SOS
 * onboardled_start_quick_flash(5, &ONBOARDLED_COLOR_YELLOW); // Yellow quick flashes
 *
 * // Priority example: Error overrides any running pattern
 * if (error_occurred) {
 *     onboardled_stop_pattern();             // Stop current pattern
 *     onboardled_start_failure(NULL);        // Start red error indication
 * }
 *
 * // Color themes for system states
 * onboardled_color_t wifi_connecting = {0, 100, 255};  // Light blue
 * onboardled_color_t wifi_connected = {0, 255, 0};     // Green
 * onboardled_start_blink(500, 500, 0, &wifi_connecting); // Infinite blink
 *
 * // Custom patterns (blocking)
 * onboardled_steady_blink(500, 10, &ONBOARDLED_COLOR_MAGENTA); // Magenta blinks
 * onboardled_dynamic_blink(100, 200, 5, &ONBOARDLED_COLOR_WHITE); // White dynamic blinks
 * onboardled_signal_code(7, 200, 100, 1000, &ONBOARDLED_COLOR_CYAN); // Cyan signal code 7
 */

#include "onboardled.h"
#include "driver/gpio.h"
#include "led_strip.h"
#ifndef CONFIG_BLINK_LED_STRIP
#include "driver/ledc.h"
#endif
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "commonutils.h"
#include <math.h>

static const char *TAG = "ONBOARDLED";

// Default GPIO pin if CONFIG_BLINK_GPIO is not defined
#ifndef CONFIG_BLINK_GPIO
#define CONFIG_BLINK_GPIO 8
#endif

// Fallback definitions for missing CONFIG symbols (for language server)
#ifndef CONFIG_LOG_MAXIMUM_LEVEL
#define CONFIG_LOG_MAXIMUM_LEVEL 3
#endif

#ifndef CONFIG_FREERTOS_HZ
#define CONFIG_FREERTOS_HZ 1000
#endif

// Module state
static uint8_t s_pin = CONFIG_BLINK_GPIO;
static bool s_active_low = false;                            // Default to active high
static uint32_t s_blink_period_ms = 1000;                    // Default blink period
static onboardled_color_t s_current_color = {255, 255, 255}; // Default white

#ifdef CONFIG_BLINK_LED_STRIP
// Handle for addressable LED strip
static led_strip_handle_t led_strip = NULL;
static bool s_strip_initialized = false;
// Timer used to perform a delayed second refresh to combat residual glow
static esp_timer_handle_t s_double_refresh_timer = NULL;
#endif

// Non-blocking pattern state machine
typedef enum
{
    PATTERN_NONE = 0,
    PATTERN_BLINK,
    PATTERN_HEARTBEAT,
    PATTERN_SOS,
    PATTERN_ERROR_CODE,
    PATTERN_SIGNAL_CODE,
    PATTERN_BURST,
    PATTERN_BUSY,
    PATTERN_QUICK_FLASH,
    PATTERN_BREATHING,
    PATTERN_CUSTOM
} pattern_type_t;

typedef enum
{
    PATTERN_STATE_IDLE = 0,
    PATTERN_STATE_ON,
    PATTERN_STATE_OFF,
    PATTERN_STATE_GAP,
    PATTERN_STATE_INTER_GROUP,
    PATTERN_STATE_INTER_LETTER,
    PATTERN_STATE_FADE_IN,
    PATTERN_STATE_FADE_OUT,
    PATTERN_STATE_PAUSE,
    PATTERN_STATE_COMPLETE
} pattern_state_t;

typedef struct
{
    pattern_type_t type;
    pattern_state_t state;
    esp_timer_handle_t timer;

    // Pattern parameters
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t gap_ms;
    uint32_t count;
    uint32_t repeats;
    uint32_t groups;

    // State tracking
    uint32_t current_count;
    uint32_t current_repeat;
    uint32_t current_group;
    uint32_t step; // For complex patterns like SOS

    // Pattern data for complex sequences
    uint8_t error_code;
    bool is_running;
    uint8_t priority;

    // Breathing pattern specific
    uint32_t fade_in_ms;
    uint32_t fade_out_ms;
    uint32_t pause_ms;
    uint32_t breathing_step_ms; // Time per brightness step
    uint8_t current_brightness; // Current brightness level (0 to BRLEVEL)
    uint8_t target_brightness;  // Target brightness for current phase
    uint32_t breathing_steps;   // Total number of steps for fade (independent of BRLEVEL)
    uint32_t current_step;      // Current step in the fade (0 to breathing_steps)

    // Color for the pattern
    onboardled_color_t color;
} pattern_context_t;

static pattern_context_t s_pattern_ctx = {0};

// Gamma LUT: maps 0..255 input -> (mapped * 256) where mapped is in 0..BRLEVEL
static uint16_t s_gamma_lut[256];
static bool s_gamma_enabled = true;   // enable gamma by default
static float s_gamma_value = 2.2f;    // perceptual gamma
static bool s_dither_enabled = false; // auto-enabled in init when BRLEVEL small

// Simple xorshift RNG (32-bit) for stochastic dithering — cheap and small
static uint32_t s_rng_state = 0xDEADBEEF;

static inline uint8_t rng8(void)
{
    // xorshift32
    s_rng_state ^= s_rng_state << 13;
    s_rng_state ^= s_rng_state >> 17;
    s_rng_state ^= s_rng_state << 5;
    return (uint8_t)(s_rng_state >> 24);
}

// Initialize gamma LUT. Each entry is (mapped_value * 256)
static void gamma_lut_init(void)
{
    if (!s_gamma_enabled)
        return;

    for (int i = 0; i < 256; ++i)
    {
        float fin = (float)i / 255.0f;
        float mout = powf(fin, s_gamma_value) * (float)BRLEVEL;
        if (mout < 0.0f)
            mout = 0.0f;
        if (mout > (float)BRLEVEL)
            mout = (float)BRLEVEL;
        uint16_t scaled = (uint16_t)(mout * 256.0f); // fractional stored in low 8 bits
        s_gamma_lut[i] = scaled;
    }
}

// Map an input channel value (0..255) to the final driver channel 0..BRLEVEL,
// applying gamma (via LUT) and optional stochastic temporal dithering.
static inline uint8_t map_channel(uint8_t in)
{
    if (!s_gamma_enabled)
    {
        // Simple linear scale: 0..255 -> 0..BRLEVEL
        if (BRLEVEL == 255)
            return in;
        return (uint8_t)(((uint32_t)in * (uint32_t)BRLEVEL + 127) / 255);
    }

    uint16_t scaled = s_gamma_lut[in];
    uint8_t iv = (uint8_t)(scaled >> 8);
    uint8_t frac = (uint8_t)(scaled & 0xFF);
    uint8_t out = iv;
    if (s_dither_enabled && frac)
    {
        uint8_t r = rng8();
        if (r < frac && out < BRLEVEL)
            out++;
    }
    if (out > BRLEVEL)
        out = BRLEVEL;
    return out;
}

// PWM (LEDC) state for single-GPIO LEDs (non-strip)
#ifndef CONFIG_BLINK_LED_STRIP
static bool s_ledc_initialized = false;
#endif

// Forward declarations for timer callbacks
static void pattern_timer_callback(void *arg);
static inline void onboardled_write(bool on);
static inline void onboardled_write_color(bool on, onboardled_color_t color);

#ifdef CONFIG_BLINK_LED_STRIP
/**
 * @brief One-shot esp_timer callback to perform a delayed second refresh of the
 * LED strip after a clear. Runs in the esp-timer task context.
 */
static void led_double_refresh_cb(void *arg)
{
    (void)arg;
    if (s_strip_initialized && led_strip)
    {
        /* Perform two refreshes spaced by a short delay to improve latching
         * behavior on problematic strips. Doing both refreshes inside this
         * callback avoids scheduling additional timers. */
        esp_err_t rc2 = led_strip_refresh(led_strip);
        if (rc2 != ESP_OK)
        {
            ESP_LOGW(TAG, "led_strip_refresh (delayed 1) failed: %d", rc2);
        }

        /* Small spacing (5 ms) between refreshes */
        esp_rom_delay_us(5000);

        esp_err_t rc3 = led_strip_refresh(led_strip);
        if (rc3 != ESP_OK)
        {
            ESP_LOGW(TAG, "led_strip_refresh (delayed 2) failed: %d", rc3);
        }
        else if (rc2 == ESP_OK)
        {
            ESP_LOGD(TAG, "led_strip: delayed double-refresh completed");
        }
    }
}
#endif

/**
 * @brief Timer callback for pattern state machine
 */
static void pattern_timer_callback(void *arg)
{
    if (!s_pattern_ctx.is_running)
    {
        return;
    }

    uint32_t next_delay_ms = 0;
    bool continue_pattern = true;

    switch (s_pattern_ctx.type)
    {
    case PATTERN_BLINK:
        if (s_pattern_ctx.state == PATTERN_STATE_OFF)
        {
            onboardled_write_color(true, s_pattern_ctx.color);
            s_pattern_ctx.state = PATTERN_STATE_ON;
            next_delay_ms = s_pattern_ctx.on_ms;
        }
        else
        {
            onboardled_write_color(false, s_pattern_ctx.color);
            s_pattern_ctx.state = PATTERN_STATE_OFF;
            s_pattern_ctx.current_count++;

            if (s_pattern_ctx.current_count >= s_pattern_ctx.count)
            {
                continue_pattern = false;
            }
            else
            {
                next_delay_ms = s_pattern_ctx.off_ms;
            }
        }
        break;

    case PATTERN_HEARTBEAT:
        switch (s_pattern_ctx.step)
        {
        case 0: // Short pulse on
            onboardled_write_color(true, s_pattern_ctx.color);
            next_delay_ms = s_pattern_ctx.on_ms; // short_ms
            s_pattern_ctx.step = 1;
            break;
        case 1: // Short pulse off
            onboardled_write_color(false, s_pattern_ctx.color);
            next_delay_ms = s_pattern_ctx.on_ms; // short_ms gap
            s_pattern_ctx.step = 2;
            break;
        case 2: // Long pulse on
            onboardled_write_color(true, s_pattern_ctx.color);
            next_delay_ms = s_pattern_ctx.off_ms; // long_ms
            s_pattern_ctx.step = 3;
            break;
        case 3: // Long pulse off, check if more beats
            onboardled_write_color(false, s_pattern_ctx.color);
            s_pattern_ctx.current_count++;
            if (s_pattern_ctx.current_count >= s_pattern_ctx.count)
            {
                continue_pattern = false;
            }
            else
            {
                next_delay_ms = s_pattern_ctx.gap_ms;
                s_pattern_ctx.step = 0;
            }
            break;
        }
        break;

    case PATTERN_SOS:
        // SOS: ... --- ... (dots=3*unit, dashes=9*unit total)
        // Steps 0-5: S (...), 6-8: O (---), 9-14: S (...)
        if (s_pattern_ctx.step < 6 || s_pattern_ctx.step >= 9)
        {
            // Dot pattern
            if (s_pattern_ctx.state == PATTERN_STATE_OFF)
            {
                onboardled_write_color(true, s_pattern_ctx.color);
                s_pattern_ctx.state = PATTERN_STATE_ON;
                next_delay_ms = s_pattern_ctx.on_ms; // unit_ms
            }
            else
            {
                onboardled_write_color(false, s_pattern_ctx.color);
                s_pattern_ctx.state = PATTERN_STATE_OFF;
                s_pattern_ctx.step++;
                if (s_pattern_ctx.step == 6 || s_pattern_ctx.step == 15)
                {
                    // Inter-letter gap or end
                    next_delay_ms = s_pattern_ctx.step == 15 ? 0 : 2 * s_pattern_ctx.on_ms;
                    continue_pattern = s_pattern_ctx.step != 15;
                }
                else
                {
                    next_delay_ms = s_pattern_ctx.on_ms;
                }
            }
        }
        else
        {
            // Dash pattern (steps 6-8)
            if (s_pattern_ctx.state == PATTERN_STATE_OFF)
            {
                onboardled_write_color(true, s_pattern_ctx.color);
                s_pattern_ctx.state = PATTERN_STATE_ON;
                next_delay_ms = 3 * s_pattern_ctx.on_ms; // 3*unit_ms
            }
            else
            {
                onboardled_write_color(false, s_pattern_ctx.color);
                s_pattern_ctx.state = PATTERN_STATE_OFF;
                s_pattern_ctx.step++;
                if (s_pattern_ctx.step == 9)
                {
                    next_delay_ms = 2 * s_pattern_ctx.on_ms; // Inter-letter gap
                }
                else
                {
                    next_delay_ms = s_pattern_ctx.on_ms;
                }
            }
        }
        break;

    case PATTERN_QUICK_FLASH:
        if (s_pattern_ctx.state == PATTERN_STATE_OFF)
        {
            onboardled_write_color(true, s_pattern_ctx.color);
            s_pattern_ctx.state = PATTERN_STATE_ON;
            next_delay_ms = 75;
        }
        else
        {
            onboardled_write_color(false, s_pattern_ctx.color);
            s_pattern_ctx.state = PATTERN_STATE_OFF;
            s_pattern_ctx.current_count++;

            if (s_pattern_ctx.current_count >= s_pattern_ctx.count)
            {
                continue_pattern = false;
            }
            else
            {
                next_delay_ms = 100;
            }
        }
        break;

    case PATTERN_BREATHING:
    {
        onboardled_color_t currstepbrightness;

        switch (s_pattern_ctx.state)
        {
        case PATTERN_STATE_FADE_IN:
        {
            // Compute fractional progress [0.0 .. 1.0]
            float frac = (float)s_pattern_ctx.current_step / (float)s_pattern_ctx.breathing_steps;

            // Interpolate color in the 0..BRLEVEL space (BRLEVEL is the hardware max)
            currstepbrightness.r = (uint8_t)((float)s_pattern_ctx.color.r * frac + 0.5f);
            currstepbrightness.g = (uint8_t)((float)s_pattern_ctx.color.g * frac + 0.5f);
            currstepbrightness.b = (uint8_t)((float)s_pattern_ctx.color.b * frac + 0.5f);

            // Write (if any channel > 0, treat as on)
            if (currstepbrightness.r || currstepbrightness.g || currstepbrightness.b)
            {
                onboardled_write_color(true, currstepbrightness);
            }
            else
            {
                onboardled_write_color(false, currstepbrightness);
            }

            // Increment step
            s_pattern_ctx.current_step++;

            if (s_pattern_ctx.current_step > s_pattern_ctx.breathing_steps)
            {
                // Fade in complete, switch to fade out
                s_pattern_ctx.state = PATTERN_STATE_FADE_OUT;
                s_pattern_ctx.current_step = s_pattern_ctx.breathing_steps; // Start fade out from max
            }

            next_delay_ms = s_pattern_ctx.breathing_step_ms;
        }
        break;

        case PATTERN_STATE_FADE_OUT:
        {
            // Compute fractional progress [0.0 .. 1.0] where 1.0 -> full brightness, 0.0 -> off
            float frac = (float)s_pattern_ctx.current_step / (float)s_pattern_ctx.breathing_steps;

            // Interpolate color in the 0..BRLEVEL space (BRLEVEL is the hardware max)
            currstepbrightness.r = (uint8_t)((float)s_pattern_ctx.color.r * frac + 0.5f);
            currstepbrightness.g = (uint8_t)((float)s_pattern_ctx.color.g * frac + 0.5f);
            currstepbrightness.b = (uint8_t)((float)s_pattern_ctx.color.b * frac + 0.5f);

            if (currstepbrightness.r || currstepbrightness.g || currstepbrightness.b)
            {
                onboardled_write_color(true, currstepbrightness);
            }
            else
            {
                onboardled_write_color(false, currstepbrightness);
            }

            // Decrement step
            if (s_pattern_ctx.current_step > 0)
                s_pattern_ctx.current_step--;

            if (s_pattern_ctx.current_step == 0)
            {
                // Fade out complete: immediately ensure LEDs are off, then
                // enter PAUSE and schedule the configured pause duration.
                // Clearing now prevents the small residual-step glow seen when
                // the clear is deferred until the PAUSE handler.
                onboardled_write_color(false, s_pattern_ctx.color);

                if (s_pattern_ctx.pause_ms > 0)
                {
                    s_pattern_ctx.state = PATTERN_STATE_PAUSE;
                    /* Count this completed cycle now */
                    s_pattern_ctx.current_count++;
                    next_delay_ms = s_pattern_ctx.pause_ms;
                }
                else
                {
                    // No pause, go directly to next cycle or complete
                    s_pattern_ctx.current_count++;
                    if (s_pattern_ctx.count == 0 || s_pattern_ctx.current_count < s_pattern_ctx.count)
                    {
                        s_pattern_ctx.state = PATTERN_STATE_FADE_IN;
                        s_pattern_ctx.current_step = 0;
                        next_delay_ms = s_pattern_ctx.breathing_step_ms;
                    }
                    else
                    {
                        continue_pattern = false;
                    }
                }
            }
            else
            {
                next_delay_ms = s_pattern_ctx.breathing_step_ms;
            }
        }
        break;

        case PATTERN_STATE_PAUSE:
            // Pause: we've previously cleared the LEDs when fade-out finished.
            // This callback is invoked after the configured pause_ms; start the
            // next fade-in step (or complete) without re-clearing or re-scheduling
            // the pause.
            ESP_LOGD(TAG, "Entering PAUSE state: pausing %lu ms (LED already cleared)", s_pattern_ctx.pause_ms);

            if (s_pattern_ctx.count == 0 || s_pattern_ctx.current_count < s_pattern_ctx.count)
            {
                s_pattern_ctx.state = PATTERN_STATE_FADE_IN;
                s_pattern_ctx.current_step = 0;
                s_pattern_ctx.current_brightness = 0;
                next_delay_ms = s_pattern_ctx.breathing_step_ms;
            }
            else
            {
                continue_pattern = false;
            }
            break;

        default:
            continue_pattern = false;
            break;
        }
    }
    break;

    default:
        continue_pattern = false;
        break;
    }

    if (continue_pattern && next_delay_ms > 0)
    {
        ESP_ERROR_CHECK(esp_timer_start_once(s_pattern_ctx.timer, next_delay_ms * 1000));
    }
    else
    {
        // Pattern complete
        onboardled_write_color(false, s_pattern_ctx.color);
        s_pattern_ctx.is_running = false;
        s_pattern_ctx.type = PATTERN_NONE;
        ESP_LOGD(TAG, "Pattern completed");
    }
}

/**
 * @brief Stop any running pattern
 */
static void stop_current_pattern(void)
{
    if (s_pattern_ctx.is_running)
    {
        esp_timer_stop(s_pattern_ctx.timer);
        s_pattern_ctx.is_running = false;
        s_pattern_ctx.type = PATTERN_NONE;
        onboardled_write(false);
    }
}

/**
 * @brief Initialize the pattern timer if not already done
 */
static esp_err_t init_pattern_timer(void)
{
    if (s_pattern_ctx.timer == NULL)
    {
        esp_timer_create_args_t timer_args = {
            .callback = pattern_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "onboardled_pattern",
            .skip_unhandled_events = true // Allow sleep during LED patterns
        };
        return esp_timer_create(&timer_args, &s_pattern_ctx.timer);
    }
    return ESP_OK;
}

// ==================== NON-BLOCKING PATTERN FUNCTIONS ====================

bool onboardled_start_blink(uint32_t on_ms, uint32_t off_ms, uint32_t count, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    if (s_pattern_ctx.is_running)
    {
        return false; // Pattern already running
    }

    if (init_pattern_timer() != ESP_OK)
    {
        return false;
    }

    // Initialize pattern context
    s_pattern_ctx.type = PATTERN_BLINK;
    s_pattern_ctx.state = PATTERN_STATE_OFF;
    s_pattern_ctx.on_ms = on_ms;
    s_pattern_ctx.off_ms = off_ms;
    s_pattern_ctx.count = count;
    s_pattern_ctx.current_count = 0;
    s_pattern_ctx.color = effective_color;
    s_pattern_ctx.is_running = true;

    // Start first blink
    onboardled_write_color(true, effective_color);
    s_pattern_ctx.state = PATTERN_STATE_ON;
    esp_timer_start_once(s_pattern_ctx.timer, on_ms * 1000);

    ESP_LOGD(TAG, "Started non-blocking blink pattern: %lu blinks", count);
    return true;
}

bool onboardled_start_heartbeat(uint32_t repeats, uint32_t short_ms, uint32_t long_ms, uint32_t gap_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    if (s_pattern_ctx.is_running)
    {
        return false;
    }

    if (init_pattern_timer() != ESP_OK)
    {
        return false;
    }

    s_pattern_ctx.type = PATTERN_HEARTBEAT;
    s_pattern_ctx.state = PATTERN_STATE_OFF;
    s_pattern_ctx.on_ms = short_ms;
    s_pattern_ctx.off_ms = long_ms;
    s_pattern_ctx.gap_ms = gap_ms;
    s_pattern_ctx.count = repeats;
    s_pattern_ctx.current_count = 0;
    s_pattern_ctx.step = 0;
    s_pattern_ctx.color = effective_color;
    s_pattern_ctx.is_running = true;

    // Start first heartbeat
    onboardled_write_color(true, effective_color);
    esp_timer_start_once(s_pattern_ctx.timer, short_ms * 1000);

    ESP_LOGD(TAG, "Started non-blocking heartbeat pattern: %lu beats", repeats);
    return true;
}

bool onboardled_start_sos(uint32_t unit_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ERROR;
    if (s_pattern_ctx.is_running)
    {
        return false;
    }

    if (init_pattern_timer() != ESP_OK)
    {
        return false;
    }

    s_pattern_ctx.type = PATTERN_SOS;
    s_pattern_ctx.state = PATTERN_STATE_OFF;
    s_pattern_ctx.on_ms = unit_ms;
    s_pattern_ctx.step = 0;
    s_pattern_ctx.color = effective_color;
    s_pattern_ctx.is_running = true;

    // Start first dot
    onboardled_write_color(true, effective_color);
    esp_timer_start_once(s_pattern_ctx.timer, unit_ms * 1000);

    ESP_LOGD(TAG, "Started non-blocking SOS pattern");
    return true;
}

bool onboardled_start_quick_flash(uint32_t count, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    if (s_pattern_ctx.is_running)
    {
        return false;
    }

    if (init_pattern_timer() != ESP_OK)
    {
        return false;
    }

    s_pattern_ctx.type = PATTERN_QUICK_FLASH;
    s_pattern_ctx.state = PATTERN_STATE_OFF;
    s_pattern_ctx.count = count;
    s_pattern_ctx.current_count = 0;
    s_pattern_ctx.color = effective_color;
    s_pattern_ctx.is_running = true;

    // Start first flash
    onboardled_write_color(true, effective_color);
    s_pattern_ctx.state = PATTERN_STATE_ON;
    esp_timer_start_once(s_pattern_ctx.timer, 75 * 1000);

    ESP_LOGD(TAG, "Started non-blocking quick flash pattern: %lu flashes", count);
    return true;
}

void onboardled_stop_pattern(void)
{
    stop_current_pattern();
    ESP_LOGD(TAG, "Stopped LED pattern");
}

bool onboardled_is_pattern_running(void)
{
    return s_pattern_ctx.is_running;
}

uint8_t onboardled_get_current_pattern(void)
{
    return (uint8_t)s_pattern_ctx.type;
}

// ==================== BLOCKING PATTERN FUNCTIONS (Legacy) ====================

/**
 * @brief Internal function to write LED state with default color.
 *
 * @param on true to turn LED on, false to turn off
 */
static inline void onboardled_write(bool on)
{
    onboardled_write_color(on, s_current_color);
}

/**
 * @brief Internal function to write LED state with specific color.
 *
 * @param on true to turn LED on, false to turn off
 * @param color RGB color to display (ignored if on is false)
 */
static inline void onboardled_write_color(bool on, onboardled_color_t color)
{
    /* Normalize input color domain: callers historically used BRLEVEL-scaled
     * macros (e.g., RED = {BRLEVEL,0,0}). The driver now expects 0..255
     * conventional inputs before applying gamma LUT. Detect the legacy
     * BRLEVEL domain when all components are <= BRLEVEL and BRLEVEL < 255,
     * then scale up to 0..255 so existing macros continue to work. */
    uint8_t in_r = color.r;
    uint8_t in_g = color.g;
    uint8_t in_b = color.b;
    uint8_t nr = in_r;
    uint8_t ng = in_g;
    uint8_t nb = in_b;
    bool scaled_from_brlevel = false;
    if (BRLEVEL < 255 && in_r <= BRLEVEL && in_g <= BRLEVEL && in_b <= BRLEVEL)
    {
        // scale: nr = round(in * 255 / BRLEVEL)
        nr = (uint8_t)(((uint32_t)in_r * 255u + (BRLEVEL / 2)) / BRLEVEL);
        ng = (uint8_t)(((uint32_t)in_g * 255u + (BRLEVEL / 2)) / BRLEVEL);
        nb = (uint8_t)(((uint32_t)in_b * 255u + (BRLEVEL / 2)) / BRLEVEL);
        scaled_from_brlevel = true;
    }

#ifdef CONFIG_BLINK_LED_STRIP
    if (s_strip_initialized && led_strip != NULL)
    {
        /* Map input colors (0..255) through gamma LUT and optional dithering
         * into the driver domain (0..BRLEVEL). This lets callers continue to
         * pass conventional 0..255 colors while the driver enforces the
         * hardware brightness ceiling and perceptual mapping. */
        uint8_t cr = map_channel(nr);
        uint8_t cg = map_channel(ng);
        uint8_t cb = map_channel(nb);

        if (on)
        {
            // Set pixel to the mapped RGB color and refresh the strip
            esp_err_t rc = led_strip_set_pixel(led_strip, 0, cr, cg, cb);
            if (rc != ESP_OK)
            {
                ESP_LOGW(TAG, "led_strip_set_pixel failed: %d", rc);
            }
            rc = led_strip_refresh(led_strip);
            if (rc != ESP_OK)
            {
                ESP_LOGW(TAG, "led_strip_refresh (on) failed: %d", rc);
            }
            if (scaled_from_brlevel)
            {
                ESP_LOGD(TAG, "LED set to R=%u G=%u B=%u (mapped) [scaled from BRLEVEL inputs %u/%u/%u]", cr, cg, cb, in_r, in_g, in_b);
            }
            else
            {
                ESP_LOGD(TAG, "LED set to R=%u G=%u B=%u (mapped)", cr, cg, cb);
            }
        }
        else
        {
            // Explicitly write zero to pixel 0 and perform refreshes (see double-refresh timer)
            esp_err_t rc = led_strip_set_pixel(led_strip, 0, 0, 0, 0);
            if (rc != ESP_OK)
            {
                ESP_LOGW(TAG, "led_strip_set_pixel (clear) failed: %d", rc);
            }
            rc = led_strip_refresh(led_strip);
            if (rc != ESP_OK)
            {
                ESP_LOGW(TAG, "led_strip_refresh (clear) failed: %d", rc);
            }

            /* Schedule a second double-refresh via esp_timer; if timer creation
             * is not yet performed, create it. The callback will perform two
             * additional refreshes spaced ~5 ms apart. */
            if (s_double_refresh_timer == NULL)
            {
                esp_timer_create_args_t targs = {
                    .callback = led_double_refresh_cb,
                    .arg = NULL,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "led_double_refresh",
                };

                if (esp_timer_create(&targs, &s_double_refresh_timer) != ESP_OK)
                {
                    s_double_refresh_timer = NULL;
                }
            }

            if (s_double_refresh_timer)
            {
                /* Ensure the latest clear wins: stop any pending one-shot and restart.
                 * This prevents esp_timer_start_once returning ESP_ERR_INVALID_STATE
                 * if the timer is already armed. */
                if (esp_timer_is_active(s_double_refresh_timer))
                {
                    esp_err_t sret = esp_timer_stop(s_double_refresh_timer);
                    if (sret != ESP_OK && sret != ESP_ERR_INVALID_STATE)
                    {
                        ESP_LOGW(TAG, "failed to stop active double-refresh timer: %s", esp_err_to_name(sret));
                    }
                }

                esp_err_t tret = esp_timer_start_once(s_double_refresh_timer, 10000);
                if (tret != ESP_OK)
                {
                    ESP_LOGW(TAG, "failed to start double-refresh timer: %s", esp_err_to_name(tret));
                    /* fallback to immediate extra refreshes */
                    rc = led_strip_refresh(led_strip);
                    if (rc != ESP_OK)
                    {
                        ESP_LOGW(TAG, "led_strip_refresh (clear 2 fallback) failed: %d", rc);
                    }
                    rc = led_strip_refresh(led_strip);
                    if (rc != ESP_OK)
                    {
                        ESP_LOGW(TAG, "led_strip_refresh (clear 3 fallback) failed: %d", rc);
                    }
                }
            }

            ESP_LOGD(TAG, "LED cleared (off)");
        }
    }
#else
    // For GPIO LED, try to use LEDC PWM if available; otherwise fall back to simple GPIO level
    if (s_ledc_initialized)
    {
        /* Map input colors (0..255) through gamma LUT and optional dithering
         * into the driver domain (0..BRLEVEL), then compute PWM duty from
         * the resulting perceived brightness (max channel). */
        uint8_t mr = map_channel(nr);
        uint8_t mg = map_channel(ng);
        uint8_t mb = map_channel(nb);

        /* Compute perceived brightness as maximum of channels (0..BRLEVEL) */
        uint32_t brightness_level = mr;
        if (mg > brightness_level)
            brightness_level = mg;
        if (mb > brightness_level)
            brightness_level = mb;

        /* Map brightness_level (0..BRLEVEL) into LEDC duty (0..max_duty) */
        uint32_t max_duty = ((1 << LEDC_TIMER_8_BIT) - 1);
        uint32_t duty = 0;
        if (BRLEVEL > 0)
        {
            duty = (brightness_level * max_duty) / BRLEVEL;
        }

        if (!on)
        {
            duty = 0;
        }

        ESP_ERROR_CHECK(ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, duty));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0));
    }
    else
    {
        // Fallback to simple GPIO on/off if PWM not configured
        bool gpio_level = s_active_low ? !on : on;
        gpio_set_level(s_pin, gpio_level ? 1 : 0);
    }
#endif
}

void onboardled_begin(uint8_t pin, bool active_low)
{
    HEAP_TRACE_START("ONBOARDLED_INIT");

    s_pin = pin;
    s_active_low = active_low;

#ifdef CONFIG_BLINK_LED_STRIP
    if (!s_strip_initialized)
    {
        led_strip_config_t strip_config = {
            .strip_gpio_num = s_pin,
            .max_leds = 1,
        };
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
        led_strip_rmt_config_t rmt_config = {
            .resolution_hz = 10 * 1000 * 1000,
            .flags.with_dma = false,
        };
        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
        led_strip_spi_config_t spi_config = {
            .spi_bus = SPI2_HOST,
            .flags.with_dma = true,
        };
        ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
#else
#error "unsupported LED strip backend"
#endif
        led_strip_clear(led_strip);
        /* Ensure physical LEDs are updated after clearing the buffer */
        led_strip_refresh(led_strip);
        s_strip_initialized = true;
    }
    else
    {
        led_strip_clear(led_strip);
        /* Make sure the clear takes effect on the strip hardware */
        led_strip_refresh(led_strip);
    }
#else
    // Configure GPIO for simple LED
    gpio_reset_pin(s_pin);
    gpio_set_direction(s_pin, GPIO_MODE_OUTPUT);

    // Try to initialize LEDC PWM on this pin for smoother brightness control
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 500,
        .clk_cfg = LEDC_AUTO_CLK,
    };

    if (ledc_timer_config(&ledc_timer) == ESP_OK)
    {
        ledc_channel_config_t ledc_channel_cfg = {
            .gpio_num = s_pin,
            .speed_mode = LEDC_HIGH_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };

        if (ledc_channel_config(&ledc_channel_cfg) == ESP_OK)
        {
            s_ledc_initialized = true;
        }
    }
#endif

    // Default to off
    onboardled_write(false);

    // Initialize pattern timer
    init_pattern_timer();

    // Initialize gamma LUT and dithering policy
    gamma_lut_init();
    // Enable stochastic dithering automatically for small BRLEVEL values
    if (BRLEVEL <= 32)
    {
        s_dither_enabled = true;
    }
    ESP_LOGD(TAG, "gamma=%f enabled=%s dither=%s BRLEVEL=%u", s_gamma_value, s_gamma_enabled ? "true" : "false", s_dither_enabled ? "true" : "false", (unsigned)BRLEVEL);

    HEAP_TRACE_END(100); // LED strip initialization may allocate driver resources
    ESP_LOGD(TAG, "Initialized LED on pin %d, active_low=%s", s_pin, active_low ? "true" : "false");
}

void onboardled_on(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : (onboardled_color_t){255, 255, 255}; // Default white
    s_current_color = effective_color;
    onboardled_write_color(true, effective_color);
}

void onboardled_off(void)
{
    onboardled_write(false);
}

void onboardled_toggle(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : (onboardled_color_t){255, 255, 255}; // Default white
    s_current_color = effective_color;
#ifdef CONFIG_BLINK_LED_STRIP
    // For LED strip, we need to track state manually
    static bool current_state = false;
    current_state = !current_state;
    onboardled_write_color(current_state, effective_color);
#else
    // For GPIO, read current level and toggle
    int current_level = gpio_get_level(s_pin);
    bool current_on = s_active_low ? (current_level == 0) : (current_level == 1);
    onboardled_write_color(!current_on, effective_color);
#endif
}

void onboardled_set_active_low(bool active_low)
{
    s_active_low = active_low;
}

void onboardled_steady_blink(uint32_t delay_ms, uint32_t blink_count, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    for (uint32_t i = 0; i < blink_count; i++)
    {
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        onboardled_write_color(false, effective_color);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void onboardled_dynamic_blink(uint32_t on_ms, uint32_t off_ms, uint32_t blink_count, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    for (uint32_t i = 0; i < blink_count; i++)
    {
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        onboardled_write_color(false, effective_color);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void onboardled_flash(uint32_t on_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    onboardled_write_color(true, effective_color);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    onboardled_write_color(false, effective_color);
}

void onboardled_quick_flash(uint32_t count, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    for (uint32_t i = 0; i < count; i++)
    {
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(75));
        onboardled_write_color(false, effective_color);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void onboardled_long_flash(uint32_t on_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    onboardled_write_color(true, effective_color);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    onboardled_write_color(false, effective_color);
}

void onboardled_success(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_SUCCESS;
    onboardled_dynamic_blink(120, 120, 3, &effective_color);
}

void onboardled_failure(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ERROR;
    onboardled_steady_blink(100, 2, &effective_color);
}

// Non-blocking convenience functions
bool onboardled_start_success(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_SUCCESS;
    return onboardled_start_blink(120, 120, 3, &effective_color);
}

bool onboardled_start_failure(onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ERROR;
    return onboardled_start_blink(100, 100, 2, &effective_color);
}

bool onboardled_start_breathing(uint32_t fade_in_ms, uint32_t fade_out_ms, uint32_t pause_ms, uint32_t cycles, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_COLOR_BLUE; // Default blue for breathing

    if (s_pattern_ctx.is_running)
    {
        return false; // Pattern already running
    }

    if (init_pattern_timer() != ESP_OK)
    {
        return false;
    }

    // Configure number of steps for smooth breathing (independent of BRLEVEL)
    uint32_t steps = 50; // Use 50 steps for maximum smoothness regardless of BRLEVEL

    // Calculate step timing based on desired fade durations
    uint32_t fade_in_step_ms = fade_in_ms / steps;
    uint32_t fade_out_step_ms = fade_out_ms / steps;

    // Use the smaller step time for consistent timing
    uint32_t step_ms = (fade_in_step_ms < fade_out_step_ms) ? fade_in_step_ms : fade_out_step_ms;
    if (step_ms < 10)
        step_ms = 10; // Minimum 10ms per step for stability

    // Initialize pattern context
    s_pattern_ctx.type = PATTERN_BREATHING;
    s_pattern_ctx.state = PATTERN_STATE_FADE_IN;
    s_pattern_ctx.count = cycles;
    s_pattern_ctx.current_count = 0;
    // Enforce color contract: clamp to BRLEVEL if caller passed larger values
    if (effective_color.r > BRLEVEL || effective_color.g > BRLEVEL || effective_color.b > BRLEVEL)
    {
        ESP_LOGW(TAG, "breathing color components exceed BRLEVEL (%u); clamping", BRLEVEL);
        if (effective_color.r > BRLEVEL)
            effective_color.r = BRLEVEL;
        if (effective_color.g > BRLEVEL)
            effective_color.g = BRLEVEL;
        if (effective_color.b > BRLEVEL)
            effective_color.b = BRLEVEL;
    }
    s_pattern_ctx.color = effective_color;

    // Breathing-specific parameters
    s_pattern_ctx.fade_in_ms = fade_in_ms;
    s_pattern_ctx.fade_out_ms = fade_out_ms;
    s_pattern_ctx.pause_ms = pause_ms;
    s_pattern_ctx.breathing_step_ms = step_ms;
    s_pattern_ctx.breathing_steps = steps;
    s_pattern_ctx.current_step = 0;
    s_pattern_ctx.current_brightness = 0;
    s_pattern_ctx.target_brightness = BRLEVEL;

    s_pattern_ctx.is_running = true;

    // Start with minimum brightness (off)
    onboardled_write_color(false, (onboardled_color_t){0, 0, 0});

    ESP_LOGI(TAG, "Breathing params: fade_in=%lums fade_out=%lums pause=%lums steps=%lu step_ms=%lu cycles=%lu", fade_in_ms, fade_out_ms, pause_ms, steps, step_ms, cycles);

    // Start the timer
    ESP_ERROR_CHECK(esp_timer_start_once(s_pattern_ctx.timer, s_pattern_ctx.breathing_step_ms * 1000));

    return true;
}

void onboardled_signal_code(uint32_t count, uint32_t on_ms, uint32_t off_ms, uint32_t gap_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    for (uint32_t i = 0; i < count; i++)
    {
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        onboardled_write_color(false, effective_color);
        if (i + 1 < count)
        {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
    vTaskDelay(pdMS_TO_TICKS(gap_ms));
}

void onboardled_burst(uint32_t groups, uint32_t per_group, uint32_t on_ms, uint32_t off_ms, uint32_t gap_between_groups_ms, onboardled_color_t *color)
{
    for (uint32_t g = 0; g < groups; g++)
    {
        onboardled_dynamic_blink(on_ms, off_ms, per_group, color);
        if (g + 1 < groups)
        {
            vTaskDelay(pdMS_TO_TICKS(gap_between_groups_ms));
        }
    }
}

void onboardled_on_for(uint32_t ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    onboardled_write_color(true, effective_color);
    vTaskDelay(pdMS_TO_TICKS(ms));
    onboardled_write_color(false, effective_color);
}

void onboardled_off_for(uint32_t ms)
{
    onboardled_write(false);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void onboardled_heartbeat(uint32_t repeats, uint32_t short_ms, uint32_t long_ms, uint32_t gap_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_NEUTRAL;
    for (uint32_t r = 0; r < repeats; r++)
    {
        // Short pulse (lub)
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(short_ms));
        onboardled_write_color(false, effective_color);
        vTaskDelay(pdMS_TO_TICKS(short_ms));

        // Long pulse (dub)
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(long_ms));
        onboardled_write_color(false, effective_color);

        if (r + 1 < repeats)
        {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }
}

static void onboardled_sos_dot(uint32_t unit_ms, onboardled_color_t color)
{
    onboardled_write_color(true, color);
    vTaskDelay(pdMS_TO_TICKS(unit_ms));
    onboardled_write_color(false, color);
    vTaskDelay(pdMS_TO_TICKS(unit_ms));
}

static void onboardled_sos_dash(uint32_t unit_ms, onboardled_color_t color)
{
    onboardled_write_color(true, color);
    vTaskDelay(pdMS_TO_TICKS(3 * unit_ms));
    onboardled_write_color(false, color);
    vTaskDelay(pdMS_TO_TICKS(unit_ms));
}

void onboardled_sos(uint32_t unit_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ERROR;
    // S: ...
    onboardled_sos_dot(unit_ms, effective_color);
    onboardled_sos_dot(unit_ms, effective_color);
    onboardled_sos_dot(unit_ms, effective_color);
    vTaskDelay(pdMS_TO_TICKS(2 * unit_ms)); // Inter-letter gap

    // O: ---
    onboardled_sos_dash(unit_ms, effective_color);
    onboardled_sos_dash(unit_ms, effective_color);
    onboardled_sos_dash(unit_ms, effective_color);
    vTaskDelay(pdMS_TO_TICKS(2 * unit_ms));

    // S: ...
    onboardled_sos_dot(unit_ms, effective_color);
    onboardled_sos_dot(unit_ms, effective_color);
    onboardled_sos_dot(unit_ms, effective_color);
}

void onboardled_error_code(uint8_t code, uint32_t repeat, uint32_t on_ms, uint32_t off_ms, uint32_t group_gap_ms, onboardled_color_t *color)
{
    if (code == 0)
    {
        onboardled_failure(color);
        return;
    }

    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ERROR;
    for (uint32_t r = 0; r < repeat; r++)
    {
        for (uint8_t i = 0; i < code; i++)
        {
            onboardled_write_color(true, effective_color);
            vTaskDelay(pdMS_TO_TICKS(on_ms));
            onboardled_write_color(false, effective_color);
            if (i + 1 < code)
            {
                vTaskDelay(pdMS_TO_TICKS(off_ms));
            }
        }
        if (r + 1 < repeat)
        {
            vTaskDelay(pdMS_TO_TICKS(group_gap_ms));
        }
    }
}

void onboardled_busy(uint32_t cycles, uint32_t on_ms, uint32_t off_ms, onboardled_color_t *color)
{
    onboardled_color_t effective_color = color ? *color : ONBOARDLED_DEFAULT_ACTIVITY;
    for (uint32_t i = 0; i < cycles; i++)
    {
        onboardled_write_color(true, effective_color);
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        onboardled_write_color(false, effective_color);
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void onboardled_test_pattern(void)
{
    ESP_LOGD(TAG, "Starting onboard LED RGB test pattern");

    // Quick startup flash (blocking for immediate feedback)
    onboardled_quick_flash(3, NULL);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Demonstrate RGB colors in sequence
    ESP_LOGD(TAG, "Testing colors: Red -> Green -> Blue");
    onboardled_flash(300, &ONBOARDLED_COLOR_RED);
    vTaskDelay(pdMS_TO_TICKS(200));
    onboardled_flash(300, &ONBOARDLED_COLOR_GREEN);
    vTaskDelay(pdMS_TO_TICKS(200));
    onboardled_flash(300, &ONBOARDLED_COLOR_BLUE);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Demonstrate non-blocking patterns with semantic colors
    ESP_LOGD(TAG, "Starting non-blocking green success pattern...");
    onboardled_start_success(NULL);

    ESP_LOGD(TAG, "Onboard LED RGB test pattern completed");
}
uint32_t onboardled_get_period_ms(void)
{
    return s_blink_period_ms;
}

void onboardled_set_period_ms(uint32_t period_ms)
{
    // Range limiting as in original blink module
    if (period_ms < BLINK_PERIOD_MIN)
    {
        period_ms = BLINK_PERIOD_MIN;
    }
    else if (period_ms > BLINK_PERIOD_MAX)
    {
        period_ms = BLINK_PERIOD_MAX;
    }
    s_blink_period_ms = period_ms;
}

// LED disco using blocking pattern, cycling through predefined special colours
void onboardled_disco(uint32_t cycles, uint32_t on_ms, uint32_t off_ms)
{
    onboardled_color_t colors[] =
        {
            PURPLE, PINK, ORCHID, TEAL, LIME, TURQUOISE, VIOLET, INDIGO, AMBER, GOLD, SALMON, OLIVE, MAROON,
            NAVY, SILVER, BRONZE, PEACH, MINT, CORAL, SALT_PEPPER, SKY_BLUE, LAVENDER, BEIGE, CREAM, PEAR, MOSS, SAND};
    size_t num_colors = sizeof(colors) / sizeof(colors[0]);

    for (uint32_t i = 0; i < cycles; i++)
    {
        for (size_t j = 0; j < num_colors; j++)
        {
            onboardled_write_color(true, colors[j]);
            vTaskDelay(pdMS_TO_TICKS(on_ms));
            onboardled_write_color(false, colors[j]);
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

// ==================== POWER MANAGEMENT FUNCTIONS ====================

// State tracking for LED strip suspend/resume
static bool s_led_strip_suspended = false;

void onboardled_suspend_led_strip(void)
{
#ifdef CONFIG_BLINK_LED_STRIP
    if (led_strip && s_strip_initialized && !s_led_strip_suspended)
    {
        ESP_LOGI(TAG, "Suspending LED strip RMT for power savings");

        // Stop any running pattern first
        onboardled_stop_pattern();

        // Ensure LED is off before suspending
        esp_err_t ret = led_strip_clear(led_strip);
        if (ret == ESP_OK)
        {
            ret = led_strip_refresh(led_strip);
        }

        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "led_strip_refresh (clear) failed: %d", ret);
        }

        s_led_strip_suspended = true;

        // The led_strip driver in ESP-IDF doesn't expose direct RMT channel control,
        // but we can delete and recreate the strip to release the RMT resources.
        // For now, just clear it - the RMT will stay enabled but in idle state.
        // A full solution would require modifying the led_strip component or
        // managing RMT channels directly.

        ESP_LOGD(TAG, "LED strip cleared for idle mode (RMT channel remains allocated)");
    }
    else if (s_led_strip_suspended)
    {
        ESP_LOGD(TAG, "LED strip already suspended, skipping");
    }
#else
    // GPIO-based LED doesn't use RMT, nothing to suspend
    ESP_LOGD(TAG, "GPIO LED - no RMT to suspend");
#endif
}

void onboardled_resume_led_strip(void)
{
#ifdef CONFIG_BLINK_LED_STRIP
    if (led_strip && s_strip_initialized && s_led_strip_suspended)
    {
        ESP_LOGI(TAG, "Resuming LED strip RMT for active mode");

        // LED strip is already initialized and RMT channel is enabled
        // Just ensure it's in a known state
        esp_err_t ret = led_strip_clear(led_strip);
        if (ret == ESP_OK)
        {
            ret = led_strip_refresh(led_strip);
        }

        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "led_strip_refresh (resume) failed: %d", ret);
        }

        s_led_strip_suspended = false;

        ESP_LOGD(TAG, "LED strip ready for active mode");
    }
    else if (!s_led_strip_suspended)
    {
        ESP_LOGD(TAG, "LED strip already active, skipping");
    }
#else
    // GPIO-based LED doesn't use RMT, nothing to resume
    ESP_LOGD(TAG, "GPIO LED - no RMT to resume");
#endif
}
