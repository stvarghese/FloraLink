#include "nodeioprotocol.h"
#include <stdint.h>
#include "esp_err.h"

esp_err_t nodeio_init(void);
void nodeio_monitor_nodeslist(void);
size_t nodeio_publish_nodeslist(char *json, size_t json_size);
void nodeio_active_nodes_ping(void);
void nodeio_process_subscription_updates(void);

// Get the number of currently connected nodes
int nodeio_get_connected_node_count(void);
