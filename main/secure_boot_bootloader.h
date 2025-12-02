/**
 * @file secure_boot_bootloader.h
 * @brief Bootloader Integration API for Secure Boot Status Tracking
 */

#pragma once

#include "secure_boot_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bootloader hook - Called after signature verification
 * 
 * This is the main integration point for custom bootloaders.
 * Call this function after verifying app firmware signature.
 * 
 * @param verification_result True if signature valid, false if invalid/unsigned
 * @return 0 if boot should continue, -1 if boot should be aborted
 */
int secboot_bootloader_on_signature_verification(bool verification_result);

/**
 * Get sanction policy to apply on verification failure
 * 
 * @return Policy (CONTINUE, RESET, or HALT)
 */
secboot_sanction_policy_t secboot_bootloader_get_policy(void);

/**
 * Apply sanction policy (reboot, halt, or continue)
 * 
 * @param policy Policy to apply (CONTINUE, RESET, HALT)
 */
void secboot_bootloader_apply_sanction(secboot_sanction_policy_t policy);

/**
 * Record boot status from bootloader (low-level write)
 * 
 * @param status SECBOOT_STATUS_OK or SECBOOT_STATUS_FAIL
 * @return 0 on success, -1 on error
 */
int secboot_bootloader_write_status(uint8_t status);

#ifdef __cplusplus
}
#endif
