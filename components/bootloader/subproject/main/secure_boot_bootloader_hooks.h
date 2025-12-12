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
     */
    int secboot_bootloader_on_signature_verification(bool verification_result);

#ifdef __cplusplus
}
#endif
