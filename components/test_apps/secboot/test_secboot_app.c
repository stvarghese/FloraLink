/**
 * @file test_secboot_app.c
 * @brief Secure Boot Test Application
 *
 * Comprehensive test application for secure boot functionality.
 * Tests boot status FIFO initialization, persistence, and operations.
 * This app is booted by the bootloader after successful signature verification.
 */

#include "secure_boot_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "test_secboot";

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

/**
 * Print boot status structure
 */
static void print_boot_status(const secboot_status_t *status, const char *label)
{
    ESP_LOGI(TAG, "\n=== Boot Status: %s ===", label);
    ESP_LOGI(TAG, "Magic: 0x%08X %s", status->magic,
             status->magic == SECBOOT_MAGIC ? "[OK]" : "[FAIL]");
    ESP_LOGI(TAG, "Boot Count: %u", status->boot_count);
    ESP_LOGI(TAG, "FIFO Size: %u/%u", status->fifo_size, CONFIG_SECURE_BOOT_FIFO_SIZE);

    if (status->fifo_size > 0)
    {
        ESP_LOGI(TAG, "FIFO Contents:");
        for (int i = 0; i < status->fifo_size; i++)
        {
            const char *status_str = status->fifo[i] == SECBOOT_STATUS_OK ? "OK" : "FAIL";
            ESP_LOGI(TAG, "  [%d]: %s", i, status_str);
        }
    }
    else
    {
        ESP_LOGI(TAG, "FIFO: Empty");
    }
}

/**
 * Test 1: Initialization
 */
static int test_init(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 1: Initialization");

    if (secboot_status_init() != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_init() failed");
        return -1;
    }

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Init");

    if (status.magic != SECBOOT_MAGIC)
    {
        ESP_LOGE(TAG, "FAIL: Magic mismatch");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: Initialization successful");
    return 0;
}

/**
 * Test 2: Push single OK entry
 */
static int test_push_ok(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 2: Push Single OK Entry");

    if (secboot_status_push(SECBOOT_STATUS_OK) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_push(OK) failed");
        return -1;
    }

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Push OK");

    if (status.fifo_size != 1 || status.fifo[0] != SECBOOT_STATUS_OK)
    {
        ESP_LOGE(TAG, "FAIL: FIFO state incorrect");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: Push OK successful");
    return 0;
}

/**
 * Test 3: Push multiple entries
 */
static int test_push_multiple(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 3: Push Multiple Entries");

    // Push FAIL, OK, FAIL
    secboot_status_push(SECBOOT_STATUS_FAIL);
    secboot_status_push(SECBOOT_STATUS_OK);
    secboot_status_push(SECBOOT_STATUS_FAIL);

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Push Multiple");

    // Should have: OK (from test 2), FAIL, OK, FAIL
    if (status.fifo_size != 4)
    {
        ESP_LOGE(TAG, "FAIL: Expected 4 entries, got %u", status.fifo_size);
        return -1;
    }

    if (status.fifo[0] != SECBOOT_STATUS_OK ||
        status.fifo[1] != SECBOOT_STATUS_FAIL ||
        status.fifo[2] != SECBOOT_STATUS_OK ||
        status.fifo[3] != SECBOOT_STATUS_FAIL)
    {
        ESP_LOGE(TAG, "FAIL: FIFO contents incorrect");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: Multiple push successful");
    return 0;
}

/**
 * Test 4: Fill FIFO to capacity
 */
static int test_fifo_full(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 4: Fill FIFO to Capacity");

    // Currently have 4 entries, fill to 10
    for (int i = 0; i < 6; i++)
    {
        secboot_status_push(SECBOOT_STATUS_OK);
    }

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "FIFO Full");

    if (status.fifo_size != CONFIG_SECURE_BOOT_FIFO_SIZE)
    {
        ESP_LOGE(TAG, "FAIL: Expected full FIFO (%u), got %u",
                 CONFIG_SECURE_BOOT_FIFO_SIZE, status.fifo_size);
        return -1;
    }

    ESP_LOGI(TAG, "PASS: FIFO filled to capacity");
    return 0;
}

/**
 * Test 5: FIFO overflow (shift left behavior)
 */
static int test_fifo_overflow(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 5: FIFO Overflow (Shift Left)");

    // FIFO is full, push new entry - should shift left and drop oldest
    secboot_status_push(SECBOOT_STATUS_FAIL);

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Overflow");

    if (status.fifo_size != CONFIG_SECURE_BOOT_FIFO_SIZE)
    {
        ESP_LOGE(TAG, "FAIL: FIFO size should remain full");
        return -1;
    }

    // Check that oldest entry (OK) was discarded and new FAIL is at [9]
    if (status.fifo[CONFIG_SECURE_BOOT_FIFO_SIZE - 1] != SECBOOT_STATUS_FAIL)
    {
        ESP_LOGE(TAG, "FAIL: New entry not at end of FIFO");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: FIFO overflow handled correctly");
    return 0;
}

/**
 * Test 6: History string generation
 */
static int test_history_string(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 6: History String Generation");

    char history[256] = "";
    if (secboot_status_get_history_string(history, sizeof(history)) < 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get_history_string() failed");
        return -1;
    }

    ESP_LOGI(TAG, "History: %s", history);

    // Verify it's not empty and contains expected pattern
    if (strlen(history) == 0 || strcmp(history, "EMPTY") == 0)
    {
        ESP_LOGE(TAG, "FAIL: History string is empty");
        return -1;
    }

    // Check for at least one status marker
    if (strstr(history, "OK") == NULL && strstr(history, "FAIL") == NULL)
    {
        ESP_LOGE(TAG, "FAIL: History doesn't contain status markers");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: History string generated correctly");
    return 0;
}

/**
 * Test 7: Clear boot status
 */
static int test_clear(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 7: Clear Boot Status");

    if (secboot_status_clear() != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_clear() failed");
        return -1;
    }

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Clear");

    if (status.fifo_size != 0 || status.boot_count != 0)
    {
        ESP_LOGE(TAG, "FAIL: Status not properly cleared");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: Status cleared successfully");
    return 0;
}

/**
 * Test 8: Re-initialize after clear
 */
static int test_reinit_after_clear(void)
{
    ESP_LOGI(TAG, "\n>>> TEST 8: Re-initialize After Clear");

    if (secboot_status_init() != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_init() after clear failed");
        return -1;
    }

    secboot_status_t status;
    if (secboot_status_get(&status) != 0)
    {
        ESP_LOGE(TAG, "FAIL: secboot_status_get() failed");
        return -1;
    }

    print_boot_status(&status, "After Re-init");

    if (status.magic != SECBOOT_MAGIC || status.boot_count == 0)
    {
        ESP_LOGE(TAG, "FAIL: Re-initialization incomplete");
        return -1;
    }

    ESP_LOGI(TAG, "PASS: Re-initialization successful");
    return 0;
}

/* ============================================================================
 * Main Test Application
 * ============================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "   Secure Boot Test Application");
    ESP_LOGI(TAG, "========================================\n");

    // Initialize NVM
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS flash needs to be erased, erasing...");
        nvs_flash_erase();
        ret = nvs_flash_init();
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize NVS flash: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "NVS initialized");

    // Run all tests
    int total = 0, passed = 0;

    total++;
    if (test_init() == 0)
        passed++;

    total++;
    if (test_push_ok() == 0)
        passed++;

    total++;
    if (test_push_multiple() == 0)
        passed++;

    total++;
    if (test_fifo_full() == 0)
        passed++;

    total++;
    if (test_fifo_overflow() == 0)
        passed++;

    total++;
    if (test_history_string() == 0)
        passed++;

    total++;
    if (test_clear() == 0)
        passed++;

    total++;
    if (test_reinit_after_clear() == 0)
        passed++;

    // Print summary
    ESP_LOGI(TAG, "\n========================================");
    ESP_LOGI(TAG, "   Test Summary");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Total: %d, Passed: %d, Failed: %d", total, passed, total - passed);

    if (passed == total)
    {
        ESP_LOGI(TAG, "✓ All tests passed!");
    }
    else
    {
        ESP_LOGE(TAG, "✗ Some tests failed");
    }

    ESP_LOGI(TAG, "\nTest application complete. Device will restart in 10 seconds...\n");
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_restart();
}
