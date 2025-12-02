/**
 * @file secure_boot_status.c
 * @brief Secure Boot Status Tracking Implementation
 * 
 * Manages boot status FIFO stored in secboot_status partition.
 * App reads, clears, and maintains history after each boot.
 */

#include "secure_boot_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "secboot_status";

#define SECBOOT_STATUS_NAMESPACE "secboot"
#define SECBOOT_STATUS_KEY "boot_status"

/* ============================================================================
 * Low-level Flash Access (direct reads/writes to secboot_status partition)
 * ============================================================================ */

/**
 * Get the secboot_status partition
 */
static const esp_partition_t *secboot_get_partition(void)
{
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS,  // Using NVS subtype for data
        "secboot_status"
    );
}

/**
 * Read boot status from partition (raw Flash read)
 */
static int secboot_partition_read(secboot_status_t *status)
{
    const esp_partition_t *part = secboot_get_partition();
    if (!part) {
        ESP_LOGE(TAG, "secboot_status partition not found");
        return -1;
    }

    esp_err_t ret = esp_partition_read(part, 0, (uint8_t *)status, sizeof(secboot_status_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read secboot_status partition: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}

/**
 * Write boot status to partition (raw Flash write with erase if needed)
 */
static int secboot_partition_write(const secboot_status_t *status)
{
    const esp_partition_t *part = secboot_get_partition();
    if (!part) {
        ESP_LOGE(TAG, "secboot_status partition not found");
        return -1;
    }

    // Erase sector before writing (4KB minimum)
    esp_err_t ret = esp_partition_erase_range(part, 0, 4096);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase secboot_status partition: %s", esp_err_to_name(ret));
        return -1;
    }

    // Write status structure
    ret = esp_partition_write(part, 0, (const uint8_t *)status, sizeof(secboot_status_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write secboot_status partition: %s", esp_err_to_name(ret));
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/**
 * Initialize boot status tracking
 * Must be called early in app_main()
 */
int secboot_status_init(void)
{
    if (!CONFIG_SECURE_BOOT_SANCTION_ENABLED) {
        ESP_LOGI(TAG, "Boot status tracking disabled");
        return 0;
    }

    secboot_status_t status;
    if (secboot_partition_read(&status) != 0) {
        ESP_LOGE(TAG, "Failed to read boot status on init");
        return -1;
    }

    // Validate magic
    if (status.magic != SECBOOT_MAGIC) {
        ESP_LOGW(TAG, "Invalid boot status magic, initializing...");
        memset(&status, 0, sizeof(status));
        status.magic = SECBOOT_MAGIC;
        status.boot_count = 1;
        status.fifo_size = 0;
        status.fifo_head = 0;
        
        if (secboot_partition_write(&status) != 0) {
            ESP_LOGE(TAG, "Failed to initialize boot status");
            return -1;
        }
    } else {
        // Increment boot count
        status.boot_count++;
        if (secboot_partition_write(&status) != 0) {
            ESP_LOGE(TAG, "Failed to update boot count");
            return -1;
        }
    }

    ESP_LOGI(TAG, "Boot status init: count=%u, history_len=%u",
             status.boot_count, status.fifo_size);
    
    return 0;
}

/**
 * Get current boot status FIFO
 */
int secboot_status_get(secboot_status_t *out_status)
{
    if (!out_status) {
        ESP_LOGE(TAG, "Invalid output buffer");
        return -1;
    }

    return secboot_partition_read(out_status);
}

/**
 * Push new boot status to FIFO (app-side logging)
 * Pops oldest if full, pushes new entry at head
 */
int secboot_status_push(uint8_t status)
{
    if (!CONFIG_SECURE_BOOT_SANCTION_ENABLED) {
        return 0;
    }

    if (status != SECBOOT_STATUS_OK && status != SECBOOT_STATUS_FAIL) {
        ESP_LOGE(TAG, "Invalid boot status: %u", status);
        return -1;
    }

    secboot_status_t sb_status;
    if (secboot_partition_read(&sb_status) != 0) {
        return -1;
    }

    // Validate magic
    if (sb_status.magic != SECBOOT_MAGIC) {
        ESP_LOGE(TAG, "Boot status structure corrupted");
        return -1;
    }

    // Push to FIFO
    if (sb_status.fifo_size < CONFIG_SECURE_BOOT_FIFO_SIZE) {
        // FIFO not full, just append
        sb_status.fifo[sb_status.fifo_head] = status;
        sb_status.fifo_head = (sb_status.fifo_head + 1) % CONFIG_SECURE_BOOT_FIFO_SIZE;
        sb_status.fifo_size++;
    } else {
        // FIFO full, overwrite oldest (circular)
        sb_status.fifo[sb_status.fifo_head] = status;
        sb_status.fifo_head = (sb_status.fifo_head + 1) % CONFIG_SECURE_BOOT_FIFO_SIZE;
    }

    // Write back
    if (secboot_partition_write(&sb_status) != 0) {
        ESP_LOGE(TAG, "Failed to push boot status");
        return -1;
    }

    ESP_LOGD(TAG, "Boot status pushed: %u (size=%u)", status, sb_status.fifo_size);
    return 0;
}

/**
 * Get human-readable boot history string
 * Example: "OK, FAIL, FAIL, OK, OK"
 */
int secboot_status_get_history_string(char *out_str, size_t max_len)
{
    if (!out_str || max_len < 3) {
        return -1;
    }

    secboot_status_t sb_status;
    if (secboot_partition_read(&sb_status) != 0) {
        snprintf(out_str, max_len, "ERROR");
        return -1;
    }

    if (sb_status.fifo_size == 0) {
        snprintf(out_str, max_len, "EMPTY");
        return 5;
    }

    // Build history string from oldest to newest
    char *ptr = out_str;
    int remaining = max_len;

    for (int i = 0; i < sb_status.fifo_size; i++) {
        int idx = (sb_status.fifo_head - sb_status.fifo_size + i) % CONFIG_SECURE_BOOT_FIFO_SIZE;
        const char *status_str = sb_status.fifo[idx] == SECBOOT_STATUS_OK ? "OK" : "FAIL";
        
        int written = snprintf(ptr, remaining, "%s%s",
                              i > 0 ? ", " : "",
                              status_str);
        
        if (written < 0 || written >= remaining) {
            // Buffer overflow, truncate
            ptr[remaining - 1] = '\0';
            return -1;
        }
        
        ptr += written;
        remaining -= written;
    }

    return ptr - out_str;
}

/**
 * Clear boot status (reset FIFO and counters)
 */
int secboot_status_clear(void)
{
    if (!CONFIG_SECURE_BOOT_SANCTION_ENABLED) {
        return 0;
    }

    secboot_status_t sb_status;
    memset(&sb_status, 0, sizeof(sb_status));
    sb_status.magic = SECBOOT_MAGIC;
    sb_status.boot_count = 0;
    sb_status.fifo_size = 0;
    sb_status.fifo_head = 0;

    if (secboot_partition_write(&sb_status) != 0) {
        ESP_LOGE(TAG, "Failed to clear boot status");
        return -1;
    }

    ESP_LOGI(TAG, "Boot status cleared");
    return 0;
}
