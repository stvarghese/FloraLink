# Secure Boot Status - Quick Start Guide

## What is Boot Status Tracking?

Boot status tracking maintains a **10-entry FIFO history** of recent boot outcomes (OK/FAIL). This allows you to:
- See patterns of boot failures
- Diagnose startup issues
- Monitor firmware stability without burning eFuses
- Integrate warnings into the web UI

The system is **reversible** and **non-intrusive**:
- No eFuses burned (can be disabled/reset anytime)
- Automatic circular buffer management
- Minimal 1KB flash footprint

## Implementation Status

✅ **Completed**:
- Boot status structure design (32 bytes packed)
- App-side implementation (secure_boot_status.c)
- Menuconfig options for flexible configuration
- Partition table with secboot_status entry
- Integration with app startup (tasks.c)
- Boot history exposed in /stats endpoint

⏳ **Pending**:
- Bootloader integration (write status on signature events)
- Frontend warning banner
- Testing with actual signed/unsigned firmware

## Quick Usage

### Enable/Configure via Menuconfig

```powershell
cd d:\ESP32_IDF\Projects\FloraLink
$env:IDF_PATH = "C:\Users\stvar\esp\v5.5\esp-idf"
. "$env:IDF_PATH\export.ps1"
idf.py menuconfig
```

Navigate to: **Secure Boot Configuration**

Options:
- `CONFIG_SECURE_BOOT_SANCTION_ENABLED` - Master enable (default: ON)
- `CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED` - Warn on unsigned (default: ON)
- `CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED` - Block unsigned (default: OFF)
- `CONFIG_SECURE_BOOT_RESET_ON_FAIL` - Reboot on failed signature (default: OFF)
- `CONFIG_SECURE_BOOT_FIFO_SIZE` - History capacity (default: 10, range: 4-32)

### Build & Flash

```powershell
idf.py build
idf.py -p COM9 flash monitor
```

### Check Boot History

**Via JSON API**:
```bash
curl http://<hub-ip>/stats
# Output includes: "boot_history": "OK, OK, OK"
```

**Via Web UI**:
- Open `http://<hub-ip>/stats`
- Boot history displayed in stats panel (see `/stats` endpoint)

### Programmatic Access

**App code**:
```c
#include "secure_boot_config.h"

// Initialization (already done in tasks.c):
secboot_status_init();

// Log boot success:
secboot_status_push(SECBOOT_STATUS_OK);

// Get history as string:
char history[256];
secboot_status_get_history_string(history, sizeof(history));
ESP_LOGI(TAG, "Boot history: %s", history);

// Get full structure:
secboot_status_t status;
secboot_status_get(&status);
ESP_LOGI(TAG, "Boot count: %u, FIFO size: %u", 
         status.boot_count, status.fifo_size);

// Reset tracking (factory reset):
secboot_status_clear();
```

## How It Works

### Boot Flow

1. **App Starts**: `secboot_status_init()` reads current FIFO from Flash
2. **Magic Check**: Validates structure integrity
3. **Boot Count**: Increments on every boot
4. **App Running**: Call `secboot_status_push()` to log boot success
5. **FIFO Management**: Automatic circular buffer (pops oldest when full)
6. **Persistence**: All changes written to `secboot_status` partition

### FIFO Ring Buffer

Example progression:
```
Start:        [empty]
Boot 1:       [OK]
Boot 2:       [OK, OK]
Boot 3:       [OK, OK, FAIL]
...
Boot 11:      [FAIL, OK, OK, OK, OK, OK, OK, OK, OK, OK]
              ^ oldest                              ^ newest
              (boot 2 discarded, boot 11 added)
```

### Sanction Policies

The system supports three boot sanction policies (for future bootloader integration):

1. **CONTINUE** (default): Log warning, allow boot
2. **RESET**: Reboot and retry on signature failure
3. **HALT**: Block boot entirely (manual intervention required)

Policy is selected based on menuconfig:
- `RESET_ON_FAIL` enabled → RESET policy
- `BLOCK_ON_UNSIGNED` enabled → HALT policy
- Otherwise → CONTINUE policy

## Integration Points

### Current Integration

| Component | Status | Details |
|-----------|--------|---------|
| `tasks.c` | ✅ | Calls `secboot_status_init()` after NVM init |
| `webserver.c` | ✅ | Exposes `boot_history` in `/stats` JSON |
| Partition Table | ✅ | 1KB `secboot_status` partition defined |
| Menuconfig | ✅ | 5 new config options available |

### Future Integration

| Component | Required | Details |
|-----------|----------|---------|
| Bootloader | ⏳ | Write status on signature verification |
| Frontend | ⏳ | Display boot history warnings |
| CLI | ⏳ | Commands to query/clear boot status |
| OTA | ⏳ | Track OTA-related boot failures |

## Files Overview

| File | Purpose |
|------|---------|
| `secure_boot_config.h` | Configuration, structure defs, public API |
| `secure_boot_status.c` | FIFO implementation, Flash access |
| `partitions_singleapp.csv` | 1KB secboot_status partition |
| `tasks.c` | Init call on app startup |
| `webserver.c` | JSON endpoint integration |
| `Kconfig.projbuild` | Menuconfig options |

## Troubleshooting

### Issue: Boot status not persisting

**Check**:
- Is partition table correctly flashed? (`idf.py partition-table-flash`)
- Is `CONFIG_SECURE_BOOT_SANCTION_ENABLED` set to y?
- Are you calling `secboot_status_push()` after app init?

### Issue: History shows "EMPTY"

**Normal if**:
- This is the first boot after partition creation
- `secboot_status_clear()` was called
- Partition was erased

**Diagnostic**:
```c
secboot_status_t status;
secboot_status_get(&status);
ESP_LOGI(TAG, "Magic: 0x%x (expect 0x%x)", status.magic, SECBOOT_MAGIC);
ESP_LOGI(TAG, "Boot count: %u", status.boot_count);
ESP_LOGI(TAG, "FIFO size: %u", status.fifo_size);
```

### Issue: Compilation errors related to partition

**Check**:
- Is `main/partitions_singleapp.csv` created?
- Is CMakeLists.txt `PARTITION_TABLE_CSV_PATH` correctly set?
- Did you run `idf.py reconfigure`?

## Next Steps

### To Complete Bootloader Integration

1. **Identify 2nd stage bootloader location**
   - ESP-IDF default: `$IDF_PATH/components/bootloader_support`
   - Custom: Check project root

2. **Modify bootloader startup sequence**
   - After signature verification, write status to secboot_status partition
   - Apply sanction policy (CONTINUE, RESET, or HALT)

3. **Test cycle**
   - Create signed and unsigned firmware versions
   - Flash each and verify boot status recorded correctly

### To Add Frontend Warning Banner

1. **Update `/stats` endpoint** (already done - includes `boot_history`)
2. **Add JavaScript** to fetch and display boot history
3. **CSS styling** for warning banner (red for failures, yellow for mixed history)
4. **Auto-refresh** history display every 5-10 seconds

### Configuration Examples

**Development (permissive)**:
```kconfig
CONFIG_SECURE_BOOT_SANCTION_ENABLED=y
CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED=y
CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED=n
CONFIG_SECURE_BOOT_RESET_ON_FAIL=n
CONFIG_SECURE_BOOT_FIFO_SIZE=10
```

**Testing (with failures)**:
```kconfig
CONFIG_SECURE_BOOT_SANCTION_ENABLED=y
CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED=y
CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED=y
CONFIG_SECURE_BOOT_RESET_ON_FAIL=n
CONFIG_SECURE_BOOT_FIFO_SIZE=10
```

**Production (strict)**:
```kconfig
CONFIG_SECURE_BOOT_SANCTION_ENABLED=y
CONFIG_SECURE_BOOT_WARN_ON_UNSIGNED=n
CONFIG_SECURE_BOOT_BLOCK_ON_UNSIGNED=y
CONFIG_SECURE_BOOT_RESET_ON_FAIL=y
CONFIG_SECURE_BOOT_FIFO_SIZE=10
```

## API Reference

### secboot_status_init()
Initializes boot status tracking. Call once on app startup.
```c
int secboot_status_init(void);
// Returns: 0 on success, -1 on error
```

### secboot_status_get()
Retrieves current boot status structure.
```c
int secboot_status_get(secboot_status_t *out_status);
// Returns: 0 on success, -1 on error
```

### secboot_status_push()
Logs a boot outcome (OK or FAIL) to FIFO.
```c
int secboot_status_push(uint8_t status);
// status: SECBOOT_STATUS_OK (0) or SECBOOT_STATUS_FAIL (1)
// Returns: 0 on success, -1 on error
```

### secboot_status_get_history_string()
Converts FIFO to human-readable string.
```c
int secboot_status_get_history_string(char *out_str, size_t max_len);
// Returns: Length of string written, -1 on error
// Example output: "OK, FAIL, FAIL, OK, OK"
```

### secboot_status_clear()
Resets all boot status tracking.
```c
int secboot_status_clear(void);
// Returns: 0 on success, -1 on error
```

## See Also

- `SECURE_BOOT_STATUS_IMPL.md` - Full implementation details
- `main/secure_boot_config.h` - Header with structure definitions
- `main/secure_boot_status.c` - Implementation source
