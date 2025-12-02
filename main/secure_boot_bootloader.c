/**
 * @file secure_boot_bootloader.c
 * @brief Bootloader Integration for Secure Boot Status Tracking
 * 
 * This module handles communication between the bootloader and app
 * regarding boot verification results and status recording.
 * 
 * The bootloader writes boot status to the secboot_status partition
 * when signature verification occurs. The app reads and maintains
 * this status on startup.
 */

#include "secure_boot_config.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <string.h>

static const char *TAG = "secboot_bl";

/**
 * Write boot status to shared partition (called by bootloader context)
 * This is typically invoked after signature verification completes.
 */
int secboot_bootloader_write_status(uint8_t status)
{
    if (status != SECBOOT_STATUS_OK && status != SECBOOT_STATUS_FAIL) {
        ESP_LOGE(TAG, "Invalid boot status: %u", status);
        return -1;
    }

    // Note: This function is designed to be called from bootloader context
    // In practice, the bootloader would call this BEFORE jumping to app
    // to record the result of signature verification.
    
    // The actual partition write happens in app-side secboot_status_push()
    // This function documents the interface contract between bootloader and app.
    
    ESP_LOGD(TAG, "Boot verification result: %s", 
             status == SECBOOT_STATUS_OK ? "OK" : "FAIL");
    
    return 0;
}

/**
 * Get sanction policy for bootloader to apply
 * Determines action on signature verification failure
 */
secboot_sanction_policy_t secboot_bootloader_get_policy(void)
{
    return secboot_get_sanction_policy();
}

/**
 * Apply sanction policy on boot failure
 * Called by bootloader if signature verification fails
 */
void secboot_bootloader_apply_sanction(secboot_sanction_policy_t policy)
{
    switch (policy) {
        case SECBOOT_SANCTION_CONTINUE:
            // Log and allow boot
            ESP_LOGW(TAG, "Unsigned firmware detected but continuing (policy: CONTINUE)");
            break;
            
        case SECBOOT_SANCTION_RESET:
            // Reboot device
            ESP_LOGW(TAG, "Signature verification failed - rebooting (policy: RESET)");
            esp_restart();
            // Never reaches here
            break;
            
        case SECBOOT_SANCTION_HALT:
            // Halt execution - requires manual intervention
            ESP_LOGE(TAG, "Signature verification failed - halting (policy: HALT)");
            ESP_LOGE(TAG, "Manually power cycle device to retry");
            while (1) {
                // Infinite loop - device must be power cycled
                ets_delay_us(1000000);  // Sleep 1s
            }
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown sanction policy: %d", policy);
            break;
    }
}

/**
 * Bootloader hook: Called by custom bootloader after signature verification
 * 
 * @param verification_result True if signature valid, false if invalid/unsigned
 * @return 0 if boot should continue, -1 if boot should be aborted
 */
int secboot_bootloader_on_signature_verification(bool verification_result)
{
    uint8_t status = verification_result ? SECBOOT_STATUS_OK : SECBOOT_STATUS_FAIL;
    
    if (!verification_result) {
        // Signature verification failed
        secboot_sanction_policy_t policy = secboot_bootloader_get_policy();
        
        if (policy == SECBOOT_SANCTION_HALT) {
            // Don't allow boot
            secboot_bootloader_apply_sanction(policy);
            return -1;
        } else if (policy == SECBOOT_SANCTION_RESET) {
            // Reboot
            secboot_bootloader_apply_sanction(policy);
            // Never returns
            return -1;
        }
        // CONTINUE: Allow boot to proceed
    }
    
    // Boot will proceed - app will record the status via secboot_status_push()
    return 0;
}
