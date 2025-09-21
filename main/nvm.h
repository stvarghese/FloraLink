/*
 * nvm_storage.h - NVM Storage for ESP32 (IDF, C)
 * Block-based NVM management with RAM shadow, CRC, and API for config/data.
 * Reference: Arduino NvmStorage class
 */
#ifndef NVM_H
#define NVM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

// Block types
typedef enum
{
    NVM_BLOCK_WIFI_CREDENTIALS = 0,
    // Future: NVM_BLOCK_WEBSOCKET_CREDENTIALS, NVM_BLOCK_APP_CONFIG, etc.
    NVM_BLOCK_COUNT
} nvm_block_type_t;

// Block info
typedef struct
{
    int addr; // Not used in NVS, but kept for reference
    int len;
    int crc_offset; // Offset from start of block
} nvm_block_info_t;

// WiFi credentials block
typedef struct
{
    char ssid[32];
    char password[64];
} nvm_wifi_credentials_block_t;

// RAM shadow for each block
extern nvm_wifi_credentials_block_t nvm_wifi_credentials_ram;

// Block info table
extern const nvm_block_info_t nvm_block_info[NVM_BLOCK_COUNT];

// API
esp_err_t nvm_init(void);
bool nvm_read_block(nvm_block_type_t block);
bool nvm_write_block(nvm_block_type_t block, const void *data); // If data==NULL, use RAM copy
void nvm_erase_block(nvm_block_type_t block);
void nvm_read_all(void);
void nvm_erase_all(void);

// CRC utility
uint32_t nvm_crc32(const void *data, size_t len);

#endif // NVM_STORAGE_H
