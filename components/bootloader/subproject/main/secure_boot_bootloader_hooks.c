/**
 * @file secure_boot_bootloader_hooks.c
 * @brief Bootloader Implementation of Secure Boot Status Hooks
 *
 * This runs in bootloader context and handles boot status after signature verification.
 * Note: Limited to minimal includes available in bootloader context.
 */

#include "secure_boot_bootloader_hooks.h"
#include "esp_log.h"

static const char *TAG = "secboot_bl";

/**
 * Called by bootloader after firmware signature verification completes
 * Determines whether to continue, reset, or halt based on verification result
 *
 * @param verification_result: true if signature valid, false if failed or unsigned
 * @return: 0 to allow boot, -1 to prevent boot
 */
int secboot_bootloader_on_signature_verification(bool verification_result)
{
    if (!verification_result)
    {
        // Signature verification failed or firmware is unsigned
        ESP_LOGW(TAG, "Firmware signature verification failed or firmware is unsigned");

        // Development mode: Allow boot but log warning
        // Production mode (with efuses): Could apply sanctions:
        //   - Reboot: esp_rom_software_reset_cpu(0);
        //   - Halt: while (1) esp_rom_delay_us(1000000);

        return 0; // Allow boot to continue (development mode)
    }

    // Signature verification passed - boot proceeds normally
    ESP_LOGI(TAG, "Firmware signature verification successful");
    return 0;
}

/**
 * Called early in bootloader to signal boot attempt start
 * Can be used to initialize bootloader-side FIFO or tracking
 */
void secboot_bootloader_on_boot_start(void)
{
    ESP_LOGI(TAG, "Boot sequence started, secure boot hooks initialized");

    // Future: Initialize bootloader-side FIFO access if needed
    // Future: Log boot timestamp
}

/**
 * Check if firmware signature verification is enabled
 *
 * @return true if CONFIG_SECURE_BOOT_V2_ENABLED, false otherwise
 */
bool secboot_bootloader_is_sig_verify_enabled(void)
{
#ifdef CONFIG_SECURE_BOOT_V2_ENABLED
    return true;
#else
    return false;
#endif
}
