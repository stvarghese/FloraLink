/*
 * nvm_storage.c - NVM Storage for ESP32 (IDF, C)
 * Implements block-based NVM management with RAM shadow, CRC, and API for config/data.
 * Reference: Arduino NvmStorage class
 */
#include "nvm.h"
#include <string.h>

#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_crc.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "NVM";

// RAM shadow for each block
nvm_wifi_credentials_block_t nvm_wifi_credentials_ram = {0};
// Door map RAM shadow
nvm_door_map_block_t nvm_door_map_ram = {0};
// Add more RAM blocks here as needed

const nvm_block_info_t nvm_block_info[NVM_BLOCK_COUNT] = {
    {0, sizeof(nvm_wifi_credentials_block_t), sizeof(nvm_wifi_credentials_block_t)},
    {0, sizeof(nvm_door_map_block_t), sizeof(nvm_door_map_block_t)},
    // Add more blocks here
};

// LUTs for RAM block pointers and NVS keys
static void *nvm_ram_block_ptrs[NVM_BLOCK_COUNT] = {
    &nvm_wifi_credentials_ram,
    &nvm_door_map_ram,
    // Add more RAM block pointers here
};

static const char *nvm_block_keys[NVM_BLOCK_COUNT] = {
    "block_wifi",
    "block_door_map",
    // Add more NVS keys here
};

static nvs_handle_t nvm_handle = 0;
// Mutex to protect RAM shadow and NVS operations
static SemaphoreHandle_t s_nvm_lock = NULL;

uint32_t nvm_crc32(const void *data, size_t len)
{
    extern uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
    return esp_crc32_le(0, (const uint8_t *)data, len);
}

esp_err_t nvm_init(void)
{
    esp_err_t err = nvs_flash_init();
    ESP_LOGI(TAG, "nvs_flash_init() returned: 0x%x (%s)", err, esp_err_to_name(err));

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        if (err == ESP_ERR_NVS_NO_FREE_PAGES)
        {
            ESP_LOGW(TAG, "NVS has no free pages - erasing and reinitializing");
        }
        else if (err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGW(TAG, "NVS version mismatch detected - erasing and reinitializing");
        }
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_LOGI(TAG, "NVS erased, attempting reinitialization...");
        err = nvs_flash_init();
        ESP_LOGI(TAG, "nvs_flash_init() after erase returned: 0x%x (%s)", err, esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(err);
    err = nvs_open("nvm", NVS_READWRITE, &nvm_handle);
    ESP_ERROR_CHECK(err);
    // Create recursive mutex for protecting RAM shadow and NVS operations
    s_nvm_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_nvm_lock)
    {
        ESP_LOGE(TAG, "Failed to create NVM recursive mutex");
        return ESP_ERR_NO_MEM;
    }
    // Read all blocks into RAM shadow so modules can read config at startup
    nvm_read_all();
    ESP_LOGI(TAG, "NVM initialized, loaded %d blocks", NVM_BLOCK_COUNT);
    return err;
}

bool nvm_read_block(nvm_block_type_t block)
{
    if (block >= NVM_BLOCK_COUNT)
        return false;
    size_t required_size = 0;
    // Ask nvs for size first by passing NULL buffer
    esp_err_t err = nvs_get_blob(nvm_handle, nvm_block_keys[block], NULL, &required_size);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Block %d not found (nvs err=%d)", block, err);
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
        return false;
    }
    if (required_size != (size_t)(nvm_block_info[block].len + sizeof(uint32_t)))
    {
        ESP_LOGW(TAG, "Block %d size mismatch (got=%u expected=%u)", block, (unsigned)required_size, (unsigned)(nvm_block_info[block].len + sizeof(uint32_t)));
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
        return false;
    }
    uint8_t *buf = (uint8_t *)malloc(required_size);
    if (!buf)
    {
        ESP_LOGE(TAG, "Out of memory reading block %d", block);
        return false;
    }
    err = nvs_get_blob(nvm_handle, nvm_block_keys[block], buf, &required_size);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to read block %d: %d", block, err);
        free(buf);
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
        return false;
    }
    // validate CRC
    uint32_t crc_stored;
    memcpy(&crc_stored, buf + nvm_block_info[block].len, sizeof(uint32_t));
    uint32_t crc_calc = nvm_crc32(buf, nvm_block_info[block].len);
    if (crc_stored != crc_calc)
    {
        ESP_LOGW(TAG, "Block %d CRC mismatch", block);
        free(buf);
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
        return false;
    }
    // copy into RAM shadow under lock
    if (s_nvm_lock)
        xSemaphoreTakeRecursive(s_nvm_lock, portMAX_DELAY);
    memcpy(nvm_ram_block_ptrs[block], buf, nvm_block_info[block].len);
    if (s_nvm_lock)
        xSemaphoreGiveRecursive(s_nvm_lock);
    free(buf);
    ESP_LOGD(TAG, "Loaded NVM block %d into RAM", block);
    return true;
}

bool nvm_write_block(nvm_block_type_t block, const void *data)
{
    if (block >= NVM_BLOCK_COUNT)
        return false;
    const void *src = data ? data : nvm_ram_block_ptrs[block];
    size_t total = nvm_block_info[block].len + sizeof(uint32_t);
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
    {
        ESP_LOGE(TAG, "Out of memory writing block %d", block);
        return false;
    }
    memcpy(buf, src, nvm_block_info[block].len);
    uint32_t crc = nvm_crc32(src, nvm_block_info[block].len);
    memcpy(buf + nvm_block_info[block].len, &crc, sizeof(uint32_t));

    // Write under lock to avoid races with other writers/readers
    if (s_nvm_lock)
        xSemaphoreTakeRecursive(s_nvm_lock, portMAX_DELAY);
    esp_err_t err = nvs_set_blob(nvm_handle, nvm_block_keys[block], buf, total);
    if (err == ESP_OK)
    {
        err = nvs_commit(nvm_handle);
    }
    if (s_nvm_lock)
        xSemaphoreGiveRecursive(s_nvm_lock);

    free(buf);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Wrote NVM block %d (%u bytes)", block, (unsigned)total);
        return true;
    }
    ESP_LOGE(TAG, "Failed to write block %d: %d", block, err);
    return false;
}

void nvm_erase_block(nvm_block_type_t block)
{
    if (block >= NVM_BLOCK_COUNT)
        return;
    if (s_nvm_lock)
        xSemaphoreTakeRecursive(s_nvm_lock, portMAX_DELAY);
    esp_err_t err = nvs_erase_key(nvm_handle, nvm_block_keys[block]);
    if (err == ESP_OK)
    {
        nvs_commit(nvm_handle);
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
        ESP_LOGD(TAG, "Erased NVM block %d", block);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to erase block %d: %d", block, err);
    }
    if (s_nvm_lock)
        xSemaphoreGiveRecursive(s_nvm_lock);
}

void nvm_read_all(void)
{
    for (int i = 0; i < NVM_BLOCK_COUNT; ++i)
    {
        if (nvm_read_block((nvm_block_type_t)i))
            ESP_LOGD(TAG, "nvm_read_all: block %d loaded", i);
        else
            ESP_LOGD(TAG, "nvm_read_all: block %d missing/zeroed", i);
    }
}

// Thread-safe shallow copy of RAM shadow block into caller-provided buffer
bool nvm_get_block(nvm_block_type_t block, void *out, size_t out_len)
{
    if (block >= NVM_BLOCK_COUNT || !out)
        return false;
    if (out_len < (size_t)nvm_block_info[block].len)
        return false;
    if (s_nvm_lock)
        xSemaphoreTakeRecursive(s_nvm_lock, portMAX_DELAY);
    memcpy(out, nvm_ram_block_ptrs[block], nvm_block_info[block].len);
    if (s_nvm_lock)
        xSemaphoreGiveRecursive(s_nvm_lock);
    return true;
}

// Thread-safe copy into RAM shadow and commit to NVS
bool nvm_set_block(nvm_block_type_t block, const void *in, size_t in_len)
{
    if (block >= NVM_BLOCK_COUNT || !in)
        return false;
    if (in_len != (size_t)nvm_block_info[block].len)
        return false;
    if (s_nvm_lock)
        xSemaphoreTakeRecursive(s_nvm_lock, portMAX_DELAY);
    memcpy(nvm_ram_block_ptrs[block], in, nvm_block_info[block].len);
    bool ok = nvm_write_block(block, NULL);
    if (s_nvm_lock)
        xSemaphoreGiveRecursive(s_nvm_lock);
    return ok;
}

void nvm_erase_all(void)
{
    for (int i = 0; i < NVM_BLOCK_COUNT; ++i)
        nvm_erase_block((nvm_block_type_t)i);
}
