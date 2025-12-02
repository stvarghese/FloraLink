#include "nodeioprotocol.h"
#include <stdint.h>
#include "esp_err.h"

esp_err_t nodeio_init(void);
void nodeio_monitor_nodeslist(void);
size_t nodeio_publish_nodeslist(char *json, size_t json_size);
void nodeio_active_nodes_ping(void);
void nodeio_process_subscription_updates(void);
// Update subscription config for a node (eg. from Web UI)
esp_err_t nodeio_update_subscription(uint8_t node_id, capability_t subscribe_mask, uint32_t interval_ms);

// Get the number of currently connected nodes
int nodeio_get_connected_node_count(void);

// Get node parameters for a specific node (returns NULL if not found)
node_params_t *nodeio_get_node_params(uint8_t node_id);

// Request logs from a node (sends MSG_LOG_REQUEST)
esp_err_t nodeio_request_logs(uint8_t node_id, uint16_t max_lines);

// Clear logs for a specific node (thread-safe, call on disconnect/error)
void nodeio_clear_node_logs(uint8_t node_id);

// Lock/unlock log access for thread-safe reads (used by webserver)
void nodeio_lock_logs(void);
void nodeio_unlock_logs(void);
