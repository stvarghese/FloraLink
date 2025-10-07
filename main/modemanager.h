#ifndef MODEMANAGER_H
#define MODEMANAGER_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

// Event group for mode signaling
extern EventGroupHandle_t g_mode_event_group;
#define MODE_ACTIVE_BIT BIT0

// Task handle management (for centralized task creation)
void modemanager_set_activity_task_handle(TaskHandle_t task_handle);

// Power/Active window management
void modemanager_enter_active(void);    // Enter active mode and start/reset timer
void modemanager_exit_active(void);     // Enter low-power mode
void modemanager_notify_activity(void); // Reset active window timer (call on node data, user action)
int modemanager_is_active(void);        // Returns 1 if in active window, 0 if low-power

// Auto sleep mode (replaces manual sleep - keeps existing functions for fallback)
esp_err_t modemanager_init_auto_light_sleep(void);    // Initialize auto light sleep configuration
void modemanager_enter_active_auto(void);             // Enter active mode with auto sleep
void modemanager_exit_active_auto(void);              // Enter idle mode with auto sleep
void modemanager_notify_activity_auto(void);          // Reset active window timer (auto sleep version)
void modemanager_notify_wkup_activity_from_isr(void); // ISR-safe activity notification

// For legacy/manual sleep (kept for comparison/fallback)
void modemanager_light_sleep(void);
void modemanager_deep_sleep(void);

// Dump pm locks
void modemanager_dump_pm_locks(void);

#endif // MODEMANAGER_H
