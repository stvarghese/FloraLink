#pragma once

/**
 * @file secure_boot_config.h
 * @brief Secure Boot Sanctions Configuration
 * 
 * Defines boot status tracking and sanction policies.
 * Configuration can be overridden via menuconfig (sdkconfig).
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Boot Status Structure (Shared between Bootloader and App)
 * ============================================================================ */

/**
 * Boot status values (binary, 0=OK, 1=FAIL)
 */
#define SECBOOT_STATUS_OK           0
#define SECBOOT_STATUS_FAIL         1

/**
 * Boot status FIFO structure stored in secboot_status partition
 * Total size: 32 bytes (easily fits in 1KB partition)
 */
typedef struct {
    uint32_t magic;              // 0x5EC0B007 (validation magic)
    uint32_t boot_count;         // Total boot attempts since creation
    uint8_t fifo[10];            // Ring buffer: 0=OK, 1=FAIL (10 entries)
    uint8_t fifo_head;           // Next write index (0-9)
    uint8_t fifo_size;           // Current FIFO occupancy (0-10)
    uint8_t reserved[5];         // Padding to 32 bytes
} __attribute__((packed)) secboot_status_t;

#define SECBOOT_MAGIC              0x5EC0B007
#define SECBOOT_FIFO_CAPACITY      10
#define SECBOOT_PARTITION_SIZE     4096  // 1KB practical minimum for wear leveling

/* ============================================================================
 * Menuconfig Defaults (can be overridden in sdkconfig)
 * ============================================================================ */

/**
 * Enable secure boot sanctions (boot status tracking and enforcement)
 * Default: Enabled (1)
 */
#ifndef CONFIG_SECURE_BOOT_SANCTION_ENABLED
#define CONFIG_SECURE_BOOT_SANCTION_ENABLED 1
#endif

/**
 * Warn on unsigned firmware (log warning but allow boot)
 * Default: Enabled (1)
 */
#ifndef CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED
#define CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED 1
#endif

/**
 * Block boot on unsigned firmware (only allow signed)
 * Default: Disabled (0) - set to 1 to enforce signed-only
 */
#ifndef CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED
#define CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED 0
#endif

/**
 * Reset on signature verification failure
 * Default: Disabled (0) - set to 1 to reboot on failed signature
 */
#ifndef CONFIG_SECURE_BOOT_RESET_ON_FAIL
#define CONFIG_SECURE_BOOT_RESET_ON_FAIL 0
#endif

/**
 * Boot sanction policy (applied by bootloader)
 * Determines action when signature verification fails or firmware is unsigned
 */
typedef enum {
    SECBOOT_SANCTION_CONTINUE = 0,   // Continue boot (log warning)
    SECBOOT_SANCTION_RESET = 1,      // Reset and retry
    SECBOOT_SANCTION_HALT = 2,       // Halt (hang) - requires manual intervention
} secboot_sanction_policy_t;

/**
 * Determine active sanction policy based on configuration
 */
static inline secboot_sanction_policy_t secboot_get_sanction_policy(void)
{
    // Priority: RESET_ON_FAIL > BLOCK_ON_UNSIGNED > default (CONTINUE)
    if (CONFIG_SECURE_BOOT_RESET_ON_FAIL) {
        return SECBOOT_SANCTION_RESET;
    }
    if (CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED) {
        return SECBOOT_SANCTION_HALT;  // Block = halt until manual action
    }
    return SECBOOT_SANCTION_CONTINUE;
}

/* ============================================================================
 * Public API (called by app on startup)
 * ============================================================================ */

/**
 * Initialize boot status tracking
 * Must be called early in app_main()
 * 
 * @return 0 on success, -1 on error
 */
int secboot_status_init(void);

/**
 * Get current boot status FIFO
 * 
 * @param out_status Pointer to secboot_status_t structure to fill
 * @return 0 on success, -1 on error
 */
int secboot_status_get(secboot_status_t *out_status);

/**
 * Push new boot status to FIFO (app-side logging)
 * Pops oldest if full, pushes new entry
 * 
 * @param status SECBOOT_STATUS_OK (0) or SECBOOT_STATUS_FAIL (1)
 * @return 0 on success, -1 on error
 */
int secboot_status_push(uint8_t status);

/**
 * Get human-readable boot history string
 * Example: "OK, FAIL, FAIL, OK, OK"
 * 
 * @param out_str Buffer to write string to
 * @param max_len Maximum length of buffer
 * @return Length of string written, -1 on error
 */
int secboot_status_get_history_string(char *out_str, size_t max_len);

/**
 * Clear boot status (reset FIFO and counters)
 * Typically called by CLI or factory reset
 * 
 * @return 0 on success, -1 on error
 */
int secboot_status_clear(void);

#ifdef __cplusplus
}
#endif
