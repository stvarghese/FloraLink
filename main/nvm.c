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

static const char *TAG = "NVM";

// RAM shadow for each block
nvm_wifi_credentials_block_t nvm_wifi_credentials_ram = {0};
// Add more RAM blocks here as needed

const nvm_block_info_t nvm_block_info[NVM_BLOCK_COUNT] = {
    {0, sizeof(nvm_wifi_credentials_block_t), sizeof(nvm_wifi_credentials_block_t)},
    // Add more blocks here
};

// LUTs for RAM block pointers and NVS keys
static void *nvm_ram_block_ptrs[NVM_BLOCK_COUNT] = {
    &nvm_wifi_credentials_ram,
    // Add more RAM block pointers here
};

static const char *nvm_block_keys[NVM_BLOCK_COUNT] = {
    "block_wifi",
    // Add more NVS keys here
};

static nvs_handle_t nvm_handle = 0;

uint32_t nvm_crc32(const void *data, size_t len)
{
    extern uint32_t esp_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
    return esp_crc32_le(0, (const uint8_t *)data, len);
}

esp_err_t nvm_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    err = nvs_open("nvm", NVS_READWRITE, &nvm_handle);
    ESP_ERROR_CHECK(err);
    return err;
}

bool nvm_read_block(nvm_block_type_t block)
{
    if (block >= NVM_BLOCK_COUNT)
        return false;
    size_t required_size = nvm_block_info[block].len + sizeof(uint32_t);
    uint8_t buf[128] = {0};
    esp_err_t err = nvs_get_blob(nvm_handle, nvm_block_keys[block], buf, &required_size);
    if (err == ESP_OK && required_size == nvm_block_info[block].len + sizeof(uint32_t))
    {
        memcpy(nvm_ram_block_ptrs[block], buf, nvm_block_info[block].len);
        uint32_t crc_stored;
        memcpy(&crc_stored, buf + nvm_block_info[block].len, sizeof(uint32_t));
        uint32_t crc_calc = nvm_crc32(nvm_ram_block_ptrs[block], nvm_block_info[block].len);
        if (crc_stored == crc_calc)
            return true;
        ESP_LOGW(TAG, "Block %d CRC mismatch", block);
    }
    else
    {
        ESP_LOGW(TAG, "Block %d not found or size mismatch", block);
    }
    memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
    return false;
}

bool nvm_write_block(nvm_block_type_t block, const void *data)
{
    if (block >= NVM_BLOCK_COUNT)
        return false;
    const void *src = data ? data : nvm_ram_block_ptrs[block];
    uint8_t buf[128] = {0};
    memcpy(buf, src, nvm_block_info[block].len);
    uint32_t crc = nvm_crc32(src, nvm_block_info[block].len);
    memcpy(buf + nvm_block_info[block].len, &crc, sizeof(uint32_t));
    esp_err_t err = nvs_set_blob(nvm_handle, nvm_block_keys[block], buf, nvm_block_info[block].len + sizeof(uint32_t));
    if (err == ESP_OK)
    {
        nvs_commit(nvm_handle);
        return true;
    }
    ESP_LOGE(TAG, "Failed to write block %d: %d", block, err);
    return false;
}

void nvm_erase_block(nvm_block_type_t block)
{
    if (block >= NVM_BLOCK_COUNT)
        return;
    esp_err_t err = nvs_erase_key(nvm_handle, nvm_block_keys[block]);
    if (err == ESP_OK)
    {
        nvs_commit(nvm_handle);
        memset(nvm_ram_block_ptrs[block], 0, nvm_block_info[block].len);
    }
    else
    {
        ESP_LOGE(TAG, "Failed to erase block %d: %d", block, err);
    }
}

void nvm_read_all(void)
{
    for (int i = 0; i < NVM_BLOCK_COUNT; ++i)
        nvm_read_block((nvm_block_type_t)i);
}

void nvm_erase_all(void)
{
    for (int i = 0; i < NVM_BLOCK_COUNT; ++i)
        nvm_erase_block((nvm_block_type_t)i);
}
