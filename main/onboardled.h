/**
 * @file onboardled.h
 * @brief Consolidated RGB onboard LED control module with pointer-based API.
 *
 * This module provides comprehensive LED control with RGB color support using an elegant
 * pointer-based API design. Single functions handle both semantic defaults (NULL pointer)
 * and custom colors (valid pointer), reducing API surface while maintaining full functionality.
 *
 * Key Features:
 * - Consolidated API: ~20 functions instead of ~40 (50% reduction)
 * - RGB Color Support: Full color control with semantic defaults
 * - Pointer-Based Design: NULL = semantic default, pointer = custom color
 * - Dual Operation Modes: Blocking and non-blocking patterns
 * - Hardware Flexibility: GPIO LEDs and addressable LED strips
 * - Backward Compatibility: Existing blink API continues to work
 *
 * Example Usage:
 * @code
 * // Initialize
 * onboardled_begin(8, false);
 *
 * // Basic operations (NULL = semantic defaults)
 * onboardled_on(NULL);                           // White
 * onboardled_success(NULL);                      // Green success
 * onboardled_failure(NULL);                      // Red failure
 *
 * // Custom colors
 * onboardled_color_t purple = {128, 0, 128};
 * onboardled_success(&purple);                   // Purple success
 *
 * // Non-blocking for responsive systems
 * onboardled_start_heartbeat(5, 100, 300, 1000, NULL);  // Blue heartbeat
 * onboardled_start_breathing(1500, 1500, 500, 0, NULL); // Blue breathing (infinite)
 * @endcode
 */

#ifndef ONBOARDLED_H
#define ONBOARDLED_H

#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

// Default GPIO pin if CONFIG_BLINK_GPIO is not defined
#ifndef CONFIG_BLINK_GPIO
#define CONFIG_BLINK_GPIO 8
#endif

// RGB Color structure
typedef struct
{
    uint8_t r; // Red component (0-255)
    uint8_t g; // Green component (0-255)
    uint8_t b; // Blue component (0-255)
} onboardled_color_t;

// Control RGB brightness level (0-255)
// NOTE: Color components passed to the API are interpreted as values in
// the range 0..BRLEVEL. BRLEVEL is the hardware channel maximum that the
// driver will send to LEDs (e.g. 10 means the driver will never send values
// greater than 10 on the 8-bit channel). Values above BRLEVEL will be
// clamped by the driver and a warning will be logged.
#define ONBOARDLED_BRIGHTNESS_LEVEL 10
#define ONBOARDLED_BRIGHTNESS_FULL 255

#define BRLEVEL ONBOARDLED_BRIGHTNESS_LEVEL
#define BRHALF (BRLEVEL * 2 / 3) // 2/3 brightness instead of 1/2 for better visibility

// Brightness scaled colour values
#define BLACK_OFF ((onboardled_color_t){0, 0, 0})                        // Black/Off
#define RED ((onboardled_color_t){BRLEVEL, 0, 0})                        // Red - Errors, failures, SOS
#define GREEN ((onboardled_color_t){0, BRLEVEL, 0})                      // Green - Success, OK
#define BLUE ((onboardled_color_t){0, 0, BRLEVEL})                       // Blue - Heartbeat, neutral operations
#define WHITE ((onboardled_color_t){BRLEVEL, BRLEVEL, BRLEVEL})          // White - Maximum brightness
#define YELLOW ((onboardled_color_t){BRLEVEL, BRLEVEL, 0})               // Yellow - Warnings
#define CYAN ((onboardled_color_t){0, BRLEVEL, BRLEVEL})                 // Cyan - Info
#define MAGENTA ((onboardled_color_t){BRLEVEL, 0, BRLEVEL})              // Magenta - Special states
#define ORANGE ((onboardled_color_t){BRLEVEL, (BRLEVEL * 165) / 255, 0}) // Orange - Activity/busy

// Other interesting colors scaled to brightness
#define PURPLE ((onboardled_color_t){BRHALF, 0, BRHALF})                                                        // Purple - Special states
#define PINK ((onboardled_color_t){BRLEVEL, BRHALF, BRHALF})                                                    // Pink - Special states
#define ORCHID ((onboardled_color_t){BRHALF, BRHALF, BRLEVEL})                                                  // Orchid - Special states
#define TEAL ((onboardled_color_t){0, BRHALF, BRHALF})                                                          // Teal - Special states
#define LIME ((onboardled_color_t){BRHALF, BRLEVEL, 0})                                                         // Lime - Special states
#define TURQUOISE ((onboardled_color_t){0, BRLEVEL, BRHALF})                                                    // Turquoise - Special states
#define VIOLET ((onboardled_color_t){BRHALF, 0, BRLEVEL})                                                       // Violet - Special states
#define INDIGO ((onboardled_color_t){BRHALF, 0, BRHALF})                                                        // Indigo - Special states
#define AMBER ((onboardled_color_t){BRLEVEL, (BRLEVEL * 191) / 255, 0})                                         // Amber - Special states
#define GOLD ((onboardled_color_t){BRLEVEL, (BRLEVEL * 215) / 255, 0})                                          // Gold - Special states
#define SALMON ((onboardled_color_t){BRLEVEL, (BRLEVEL * 128) / 255, (BRLEVEL * 114) / 255})                    // Salmon - Special states
#define OLIVE ((onboardled_color_t){BRHALF, BRHALF, 0})                                                         // Olive - Special states
#define MAROON ((onboardled_color_t){BRHALF, 0, 0})                                                             // Maroon - Special states
#define NAVY ((onboardled_color_t){0, 0, BRHALF})                                                               // Navy - Special states
#define SILVER ((onboardled_color_t){(BRLEVEL * 192) / 255, (BRLEVEL * 192) / 255, (BRLEVEL * 192) / 255})      // Silver - Special states
#define BRONZE ((onboardled_color_t){(BRLEVEL * 205) / 255, (BRLEVEL * 127) / 255, (BRLEVEL * 50) / 255})       // Bronze - Special states
#define PEACH ((onboardled_color_t){BRLEVEL, (BRLEVEL * 218) / 255, (BRLEVEL * 185) / 255})                     // Peach - Special states
#define MINT ((onboardled_color_t){(BRLEVEL * 189) / 255, BRLEVEL, (BRLEVEL * 189) / 255})                      // Mint - Special states
#define CORAL ((onboardled_color_t){BRLEVEL, (BRLEVEL * 127) / 255, (BRLEVEL * 80) / 255})                      // Coral - Special states
#define SALT_PEPPER ((onboardled_color_t){(BRLEVEL * 220) / 255, (BRLEVEL * 220) / 255, (BRLEVEL * 220) / 255}) // Salt & Pepper - Special states
#define SKY_BLUE ((onboardled_color_t){(BRLEVEL * 135) / 255, (BRLEVEL * 206) / 255, BRLEVEL})                  // Sky Blue - Special states
#define LAVENDER ((onboardled_color_t){(BRLEVEL * 230) / 255, (BRLEVEL * 230) / 255, BRLEVEL})                  // Lavender - Special states
#define BEIGE ((onboardled_color_t){(BRLEVEL * 245) / 255, (BRLEVEL * 245) / 255, (BRLEVEL * 220) / 255})       // Beige - Special states
#define CREAM ((onboardled_color_t){(BRLEVEL * 255) / 255, (BRLEVEL * 253) / 255, (BRLEVEL * 208) / 255})       // Cream - Special states
#define PEAR ((onboardled_color_t){(BRLEVEL * 209) / 255, (BRLEVEL * 226) / 255, (BRLEVEL * 49) / 255})         // Pear - Special states
#define MOSS ((onboardled_color_t){(BRLEVEL * 173) / 255, (BRLEVEL * 223) / 255, (BRLEVEL * 173) / 255})        // Moss - Special states
#define SAND ((onboardled_color_t){(BRLEVEL * 194) / 255, (BRLEVEL * 178) / 255, (BRLEVEL * 128) / 255})        // Sand - Special states
#define COCOA ((onboardled_color_t){(BRLEVEL * 210) / 255, (BRLEVEL * 105) / 255, (BRLEVEL * 30) / 255})        // Cocoa - Special states
#define CHOCOLATE ((onboardled_color_t){(BRLEVEL * 123) / 255, (BRLEVEL * 63) / 255, (BRLEVEL * 0) / 255})      // Chocolate - Special states
#define TAN ((onboardled_color_t){(BRLEVEL * 210) / 255, (BRLEVEL * 180) / 255, (BRLEVEL * 140) / 255})         // Tan - Special states
#define PEWTER ((onboardled_color_t){(BRLEVEL * 128) / 255, (BRLEVEL * 128) / 255, (BRLEVEL * 128) / 255})      // Pewter - Special states
#define SLATE ((onboardled_color_t){(BRLEVEL * 112) / 255, (BRLEVEL * 128) / 255, (BRLEVEL * 144) / 255})       // Slate - Special states
#define STEEL ((onboardled_color_t){(BRLEVEL * 70) / 255, (BRLEVEL * 130) / 255, (BRLEVEL * 180) / 255})        // Steel - Special states
#define CHARCOAL ((onboardled_color_t){(BRLEVEL * 54) / 255, (BRLEVEL * 69) / 255, (BRLEVEL * 79) / 255})       // Charcoal - Special states
#define ASPHALT ((onboardled_color_t){(BRLEVEL * 47) / 255, (BRLEVEL * 79) / 255, (BRLEVEL * 79) / 255})        // Asphalt - Special states
#define ONYX ((onboardled_color_t){(BRLEVEL * 53) / 255, (BRLEVEL * 56) / 255, (BRLEVEL * 57) / 255})           // Onyx - Special states
#define EBONY ((onboardled_color_t){(BRLEVEL * 85) / 255, (BRLEVEL * 93) / 255, (BRLEVEL * 80) / 255})          // Ebony - Special states
#define IVORY ((onboardled_color_t){(BRLEVEL * 255) / 255, (BRLEVEL * 255) / 255, (BRLEVEL * 240) / 255})       // Ivory - Special states
#define ALMOND ((onboardled_color_t){(BRLEVEL * 239) / 255, (BRLEVEL * 222) / 255, (BRLEVEL * 205) / 255})      // Almond - Special states
#define MOCHA ((onboardled_color_t){(BRLEVEL * 191) / 255, (BRLEVEL * 154) / 255, (BRLEVEL * 107) / 255})       // Mocha - Special states
#define FUCHSIA ((onboardled_color_t){BRLEVEL, 0, BRLEVEL})                                                     // Fuchsia - Special states
#define LILAC ((onboardled_color_t){(BRLEVEL * 200) / 255, (BRLEVEL * 162) / 255, (BRLEVEL * 200) / 255})       // Lilac - Special states
#define CERULEAN ((onboardled_color_t){(BRLEVEL * 42) / 255, (BRLEVEL * 82) / 255, (BRLEVEL * 190) / 255})      // Cerulean - Special states
#define PERIWINKLE ((onboardled_color_t){(BRLEVEL * 204) / 255, (BRLEVEL * 204) / 255, BRLEVEL})                // Periwinkle - Special states
#define MAUVE ((onboardled_color_t){(BRLEVEL * 224) / 255, (BRLEVEL * 176) / 255, (BRLEVEL * 255) / 255})       // Mauve - Special states
#define RUST ((onboardled_color_t){(BRLEVEL * 183) / 255, (BRLEVEL * 65) / 255, (BRLEVEL * 14) / 255})          // Rust - Special states
#define BURGUNDY ((onboardled_color_t){(BRLEVEL * 128) / 255, 0, (BRLEVEL * 32) / 255})                         // Burgundy - Special states
#define WINE ((onboardled_color_t){(BRLEVEL * 114) / 255, 0, (BRLEVEL * 47) / 255})                             // Wine - Special states
#define BRICK ((onboardled_color_t){(BRLEVEL * 156) / 255, (BRLEVEL * 52) / 255, (BRLEVEL * 31) / 255})         // Brick - Special states
#define CHESTNUT ((onboardled_color_t){(BRLEVEL * 205) / 255, (BRLEVEL * 92) / 255, (BRLEVEL * 92) / 255})      // Chestnut - Special states
#define SANGRIA ((onboardled_color_t){(BRLEVEL * 146) / 255, 0, (BRLEVEL * 10) / 255})                          // Sangria - Special states
#define CRIMSON ((onboardled_color_t){(BRLEVEL * 220) / 255, (BRLEVEL * 20) / 255, (BRLEVEL * 60) / 255})       // Crimson - Special states

// Predefined colors for semantic meaning, with brightness applied
#define ONBOARDLED_COLOR_OFF (BLACK_OFF)   // Black/Off
#define ONBOARDLED_COLOR_RED (RED)         // Red - Errors, failures, SOS
#define ONBOARDLED_COLOR_GREEN (GREEN)     // Green - Success, OK
#define ONBOARDLED_COLOR_BLUE (BLUE)       // Blue - Heartbeat, neutral operations
#define ONBOARDLED_COLOR_WHITE (WHITE)     // White - Maximum brightness
#define ONBOARDLED_COLOR_YELLOW (YELLOW)   // Yellow - Warnings
#define ONBOARDLED_COLOR_CYAN (CYAN)       // Cyan - Info
#define ONBOARDLED_COLOR_MAGENTA (MAGENTA) // Magenta - Special states
#define ONBOARDLED_COLOR_ORANGE (ORANGE)   // Orange - Activity/busy

// Default colors for semantic functions
#define ONBOARDLED_DEFAULT_ERROR ONBOARDLED_COLOR_RED
#define ONBOARDLED_DEFAULT_SUCCESS ONBOARDLED_COLOR_GREEN
#define ONBOARDLED_DEFAULT_NEUTRAL ONBOARDLED_COLOR_BLUE
#define ONBOARDLED_DEFAULT_ACTIVITY ONBOARDLED_COLOR_ORANGE

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize the onboard LED module.
     *
     * @param pin GPIO pin number for the LED (use CONFIG_BLINK_GPIO or custom pin)
     * @param active_low true if LED is active low (lights when GPIO is LOW), false if active high
     */
    void onboardled_begin(uint8_t pin, bool active_low);

    /**
     * @brief Turn the LED on with optional color.
     *
     * @param color Pointer to RGB color (NULL = default white)
     */
    void onboardled_on(onboardled_color_t *color);

    /**
     * @brief Turn the LED off.
     */
    void onboardled_off(void);

    /**
     * @brief Toggle the LED state with optional color.
     *
     * @param color Pointer to RGB color (NULL = default white)
     */
    void onboardled_toggle(onboardled_color_t *color);

    /**
     * @brief Set the active low configuration.
     *
     * @param active_low true if LED should be active low, false for active high
     */
    void onboardled_set_active_low(bool active_low);

    /**
     * @brief Simple blink pattern with equal on/off times.
     *
     * @param delay_ms Time in milliseconds for both on and off periods
     * @param blink_count Number of blinks to perform
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_steady_blink(uint32_t delay_ms, uint32_t blink_count, onboardled_color_t *color);

    /**
     * @brief Dynamic blink pattern with separate on/off times.
     *
     * @param on_ms Time in milliseconds to keep LED on
     * @param off_ms Time in milliseconds to keep LED off
     * @param blink_count Number of blinks to perform
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_dynamic_blink(uint32_t on_ms, uint32_t off_ms, uint32_t blink_count, onboardled_color_t *color);

    /**
     * @brief Flash the LED once for a specified duration.
     *
     * @param on_ms Time in milliseconds to keep LED on (then turns off)
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_flash(uint32_t on_ms, onboardled_color_t *color);

    /**
     * @brief Quick flash pattern (75ms on, 100ms off).
     *
     * @param count Number of quick flashes to perform
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_quick_flash(uint32_t count, onboardled_color_t *color);

    /**
     * @brief Long flash - turns on for specified time then off.
     *
     * @param on_ms Time in milliseconds to keep LED on
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_long_flash(uint32_t on_ms, onboardled_color_t *color);

    /**
     * @brief Success pattern (3 blinks with 120ms on/off).
     *
     * @param color Pointer to RGB color (NULL = default green for success)
     */
    void onboardled_success(onboardled_color_t *color);

    /**
     * @brief Failure pattern (2 blinks with 100ms on/off).
     *
     * @param color Pointer to RGB color (NULL = default red for errors)
     */
    void onboardled_failure(onboardled_color_t *color);

    /**
     * @brief Signal code pattern - flashes a specific count with timing.
     *
     * @param count Number of flashes in the signal
     * @param on_ms Time in milliseconds for each flash
     * @param off_ms Time in milliseconds between flashes
     * @param gap_ms Time in milliseconds after the complete signal
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_signal_code(uint32_t count, uint32_t on_ms, uint32_t off_ms, uint32_t gap_ms, onboardled_color_t *color);

    /**
     * @brief Burst pattern - groups of flashes with gaps between groups.
     *
     * @param groups Number of groups to flash
     * @param per_group Number of flashes per group
     * @param on_ms Time in milliseconds for each flash
     * @param off_ms Time in milliseconds between flashes within a group
     * @param gap_between_groups_ms Time in milliseconds between groups
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_burst(uint32_t groups, uint32_t per_group, uint32_t on_ms, uint32_t off_ms, uint32_t gap_between_groups_ms, onboardled_color_t *color);

    /**
     * @brief Keep LED on for specified time then turn off.
     *
     * @param ms Time in milliseconds to keep LED on
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_on_for(uint32_t ms, onboardled_color_t *color);

    /**
     * @brief Keep LED off for specified time.
     *
     * @param ms Time in milliseconds to keep LED off
     */
    void onboardled_off_for(uint32_t ms);

    /**
     * @brief Heartbeat pattern (lub-dub).
     *
     * @param repeats Number of heartbeat cycles to perform
     * @param short_ms Duration of short pulse (lub)
     * @param long_ms Duration of long pulse (dub)
     * @param gap_ms Gap between heartbeat cycles
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     */
    void onboardled_heartbeat(uint32_t repeats, uint32_t short_ms, uint32_t long_ms, uint32_t gap_ms, onboardled_color_t *color);

    /**
     * @brief SOS pattern (... --- ...).
     *
     * @param unit_ms Base unit time for dots and dashes
     * @param color Pointer to RGB color (NULL = default red for emergencies)
     */
    void onboardled_sos(uint32_t unit_ms, onboardled_color_t *color);

    /**
     * @brief Error code pattern - blinks a number to indicate error code.
     *
     * @param code Error code to blink (0 shows failure pattern)
     * @param repeat Number of times to repeat the error code
     * @param on_ms Time in milliseconds for each flash
     * @param off_ms Time in milliseconds between flashes
     * @param group_gap_ms Time in milliseconds between repetitions
     * @param color Pointer to RGB color (NULL = default red for errors)
     */
    void onboardled_error_code(uint8_t code, uint32_t repeat, uint32_t on_ms, uint32_t off_ms, uint32_t group_gap_ms, onboardled_color_t *color);

    /**
     * @brief Busy pattern - continuous blinking to indicate activity.
     *
     * @param cycles Number of on/off cycles to perform
     * @param on_ms Time in milliseconds for on period
     * @param off_ms Time in milliseconds for off period
     * @param color Pointer to RGB color (NULL = default orange for activity)
     */
    void onboardled_busy(uint32_t cycles, uint32_t on_ms, uint32_t off_ms, onboardled_color_t *color);

    /**
     * @brief Test pattern to demonstrate onboard LED functionality.
     *
     * Performs a startup sequence: 3 quick flashes, then success pattern, then SOS.
     * Useful for testing that the LED is working properly.
     */
    void onboardled_test_pattern(void);

    // ==================== NON-BLOCKING PATTERN FUNCTIONS ====================

    /**
     * @brief Start a non-blocking blink pattern.
     *
     * @param on_ms Time in milliseconds for on state
     * @param off_ms Time in milliseconds for off state
     * @param count Number of blinks to perform
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_blink(uint32_t on_ms, uint32_t off_ms, uint32_t count, onboardled_color_t *color);

    /**
     * @brief Start a non-blocking heartbeat pattern.
     *
     * @param repeats Number of heartbeat cycles
     * @param short_ms Duration of short pulse (lub)
     * @param long_ms Duration of long pulse (dub)
     * @param gap_ms Gap between heartbeat cycles
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_heartbeat(uint32_t repeats, uint32_t short_ms, uint32_t long_ms, uint32_t gap_ms, onboardled_color_t *color);

    /**
     * @brief Start a non-blocking SOS pattern.
     *
     * @param unit_ms Base unit time for dots and dashes
     * @param color Pointer to RGB color (NULL = default red for emergencies)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_sos(uint32_t unit_ms, onboardled_color_t *color);

    /**
     * @brief Start a non-blocking quick flash pattern.
     *
     * @param count Number of quick flashes to perform
     * @param color Pointer to RGB color (NULL = default blue for neutral operations)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_quick_flash(uint32_t count, onboardled_color_t *color);

    /**
     * @brief Start a non-blocking success pattern (3 blinks, 120ms each).
     *
     * @param color Pointer to RGB color (NULL = default green for success)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_success(onboardled_color_t *color);

    /**
     * @brief Start a non-blocking failure pattern (2 blinks, 100ms each).
     *
     * @param color Pointer to RGB color (NULL = default red for errors)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_failure(onboardled_color_t *color);

    /**
     * @brief Start a non-blocking smooth breathing pattern with PWM-like brightness control.
     *
     * Creates a smooth fade-in/fade-out effect by gradually changing brightness from 0 to BRLEVEL.
     * The pattern smoothly transitions between minimum (0) and maximum (BRLEVEL) brightness levels
     * using fine-grained brightness steps for a smooth PWM-like breathing effect.
     *
     * @param fade_in_ms Duration for fade-in transition (0 to BRLEVEL brightness)
     * @param fade_out_ms Duration for fade-out transition (BRLEVEL to 0 brightness)
     * @param pause_ms Pause duration at minimum brightness between cycles
     * @param cycles Number of breathing cycles (0 = infinite)
     * @param color Pointer to RGB color (NULL = default blue for breathing)
     * @return true if pattern started successfully, false if another pattern is running
     */
    bool onboardled_start_breathing(uint32_t fade_in_ms, uint32_t fade_out_ms, uint32_t pause_ms, uint32_t cycles, onboardled_color_t *color);

    /**
     * @brief Stop any currently running pattern.
     */
    void onboardled_stop_pattern(void);

    /**
     * @brief Check if a pattern is currently running.
     *
     * @return true if a pattern is active, false otherwise
     */
    bool onboardled_is_pattern_running(void);

    /**
     * @brief Get the type of currently running pattern.
     *
     * @return Pattern type (0 = none, 1 = blink, 2 = heartbeat, 3 = SOS, etc.)
     */
    uint8_t onboardled_get_current_pattern(void);

    /**
     * @brief Get the current blink period in ms (for compatibility with existing blink module).
     */
    uint32_t onboardled_get_period_ms(void);

    /**
     * @brief Set the blink period in ms (for compatibility with existing blink module).
     *
     * @param period_ms Blink period in milliseconds
     */
    void onboardled_set_period_ms(uint32_t period_ms);

    /**
     * @brief Disco pattern - rapid on/off multi-special colour cycles for a fun effect.
     *
     * @param cycles Number of on/off cycles to perform
     * @param on_ms Time in milliseconds for on period
     * @param off_ms Time in milliseconds for off period
     */
    void onboardled_disco(uint32_t cycles, uint32_t on_ms, uint32_t off_ms);

    // ==================== POWER MANAGEMENT FUNCTIONS ====================

    /**
     * @brief Suspend LED strip RMT peripheral to save power during idle mode.
     *
     * Disables the LED strip RMT channel and releases the APB_FREQ_MAX power
     * management lock, allowing auto light sleep to function properly.
     * Call this before entering idle/sleep mode.
     */
    void onboardled_suspend_led_strip(void);

    /**
     * @brief Resume LED strip RMT peripheral after idle mode.
     *
     * Re-enables the LED strip RMT channel for normal operation.
     * Call this when returning to active mode.
     */
    void onboardled_resume_led_strip(void);

// Compatibility macros for existing blink API
#define blink_init() onboardled_begin(CONFIG_BLINK_GPIO, false)
#define blink_toggle() onboardled_toggle(NULL)
#define blink_get_period_ms() onboardled_get_period_ms()
#define blink_set_period_ms(period) onboardled_set_period_ms(period)

// Expose min/max blink period for use in UI and backend
#define BLINK_PERIOD_MIN 100
#define BLINK_PERIOD_MAX 10000

#ifdef __cplusplus
}
#endif

#endif // ONBOARDLED_H