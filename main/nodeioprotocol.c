#include "nodeioprotocol.h"

const char MSG_TYP_CONNECT[] = "connect";
const char MSG_TYP_CONNECT_RESPONSE[] = "connect_response";
const char MSG_TYP_NODE_DATA[] = "node_data";
const char MSG_TYP_SUBSCRIBE[] = "notify_subscription";
const char MSG_TYP_POLL_DATA[] = "poll_data";
const char MSG_TYP_OTA_REQUEST[] = "ota_request";
const char MSG_TYP_OTA_STATUS[] = "ota_status";
const char MSG_TYP_DIAGNOSTIC[] = "diagnostics";
const char MSG_TYP_DIAGNOSTIC_REQUEST[] = "diagnostic_request";
const char MSG_TYP_ACK[] = "ack";
const char MSG_TYP_HEARTBEAT[] = "heartbeat";
const char MSG_TYP_PING[] = "ping";
const char MSG_TYP_PONG[] = "pong";
const char MSG_TYP_DISCONNECT_REQUEST[] = "disconnect_request";
const char MSG_TYP_ERROR[] = "error";
const char MSG_TYP_UNKNOWN[] = "unknown";

const char MSG_PAYLOAD_TYPE_SENSOR[] = "sensor";
const char MSG_PAYLOAD_TYPE_DIAGNOSTIC[] = "diagnostics";
const char MSG_PAYLOAD_TYPE_OTA_STATUS[] = "ota_status";
