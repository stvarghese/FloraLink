/**
 * @file secure_boot_bootloader_hooks.h
 * @brief Bootloader Secure Boot Status Hooks
 *
 * Integration points for secure boot status tracking in bootloader.
 * These functions are called by the bootloader after verifying firmware signatures.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Called by bootloader after firmware signature verification
     *
     * @param verification_result: true if signature valid, false if failed/unsigned
     * @return: 0 to continue boot, -1 to abort boot
     *
     * Notes:
     * - In development mode (no efuses): Always returns 0 (allow boot)
     * - In production (efuses burned): Can apply sanctions (reboot/halt/etc)
     * - Logged to ESP_LOG and can be captured via serial output
     */
    int secboot_bootloader_on_signature_verification(bool verification_result);

    /**
     * Called early in bootloader startup
     * Can be used to initialize bootloader-side tracking
     */
    void secboot_bootloader_on_boot_start(void);

    /**
     * Check if firmware signature verification is enabled in config
     *
     * @return true if CONFIG_SECURE_BOOT_V2_ENABLED, false otherwise
     */
    bool secboot_bootloader_is_sig_verify_enabled(void);

#ifdef __cplusplus
}
#endif
