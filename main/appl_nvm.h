#ifndef APPL_NVM_H
#define APPL_NVM_H

#include <stdbool.h>
#include <stddef.h>
#include "nvm.h"

// Application-level helpers for the door map
bool appl_nvm_get_door_map(nvm_door_map_block_t *out);
bool appl_nvm_set_door_map(const nvm_door_map_block_t *in);

bool appl_nvm_get_door_name(int idx, char *out, size_t out_len);
bool appl_nvm_set_door_name(int idx, const char *name);

#endif // APPL_NVM_H
