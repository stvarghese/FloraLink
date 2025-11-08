#include "appl_nvm.h"
#include <string.h>
#include <esp_log.h>

#define DOOR_MAP_VERSION 1

static const char *TAG = "APPL_NVM";

bool appl_nvm_get_door_map(nvm_door_map_block_t *out)
{
    if (!out)
        return false;
    bool ok = nvm_get_block(NVM_BLOCK_DOOR_MAP, out, sizeof(*out));
    if (!ok)
    {
        // If no block present, zero out and return true (treat as empty map)
        memset(out, 0, sizeof(*out));
        out->version = DOOR_MAP_VERSION;
        ESP_LOGD(TAG, "Door map not present - returning empty map");
        return true;
    }
    // Validate version
    if (out->version != DOOR_MAP_VERSION)
    {
        ESP_LOGW(TAG, "Door map version mismatch (got %d, expected %d) - using defaults",
                 out->version, DOOR_MAP_VERSION);
        memset(out, 0, sizeof(*out));
        out->version = DOOR_MAP_VERSION;
        return true;
    }

    // Log what we loaded
    ESP_LOGD(TAG, "Loaded door map from NVM (version %d):", out->version);
    for (int i = 0; i < NUM_DOOR_SENSORS; ++i)
    {
        ESP_LOGD(TAG, "  Door %d: '%s'", i, out->names[i]);
    }

    return true;
}

bool appl_nvm_set_door_map(const nvm_door_map_block_t *in)
{
    if (!in)
        return false;

    // Log what we're about to save
    ESP_LOGD(TAG, "Saving door map to NVM:");
    for (int i = 0; i < NUM_DOOR_SENSORS; ++i)
    {
        ESP_LOGD(TAG, "  Door %d: '%s'", i, in->names[i]);
    }

    // Copy input and set version
    nvm_door_map_block_t tmp = *in;
    tmp.version = DOOR_MAP_VERSION;

    bool ok = nvm_set_block(NVM_BLOCK_DOOR_MAP, &tmp, sizeof(tmp));
    if (!ok)
    {
        ESP_LOGE(TAG, "Failed to set door map");
        return false;
    }
    ESP_LOGI(TAG, "Door map updated in NVM (version %d)", DOOR_MAP_VERSION);
    return true;
}

bool appl_nvm_get_door_name(int idx, char *out, size_t out_len)
{
    if (!out || idx < 0 || idx >= NUM_DOOR_SENSORS)
        return false;
    nvm_door_map_block_t tmp;
    if (!appl_nvm_get_door_map(&tmp))
        return false;
    size_t copy_len = strnlen(tmp.names[idx], DOOR_NAME_LEN - 1);
    if (out_len == 0)
        return false;
    if (copy_len >= out_len)
        copy_len = out_len - 1;
    memcpy(out, tmp.names[idx], copy_len);
    out[copy_len] = '\0';
    return true;
}

bool appl_nvm_set_door_name(int idx, const char *name)
{
    if (!name || idx < 0 || idx >= NUM_DOOR_SENSORS)
        return false;
    nvm_door_map_block_t tmp;
    // load current map (may be empty)
    appl_nvm_get_door_map(&tmp);
    // copy name into slot
    memset(tmp.names[idx], 0, DOOR_NAME_LEN);
    strncpy(tmp.names[idx], name, DOOR_NAME_LEN - 1);
    return appl_nvm_set_door_map(&tmp);
}
