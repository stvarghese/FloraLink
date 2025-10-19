#include "nodeio_services.h"
#include "nodeio.h"
#include "nodeioprotocol.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"

static const char *TAG = "nodeio_services";

/*
 * services_unified_lut
 * - Private table describing service names -> locations and types in the
 *   packed protocol message structs. Like the sensors LUT this table is
 *   low-level and must be kept in sync with the corresponding C structs.
 *
 * Maintenance guidelines:
 * - Add new service entries here when introducing new service payloads.
 * - Keep the JSON key (`name`) stable: nodes use this string when sending
 *   payloads. Changing the name requires coordinated updates to node code.
 * - The LUT is intentionally read-only at runtime to avoid concurrency issues.
 */
static const field_lookup_t services_unified_lut[] = {
    {"diagnostics", CAP_DIAG, FIELD_TYPE_STRUCT, FIELD_LOC_PERIODIC, offsetof(service_payload_t, diagnostics), 0},
    {"ota_status", CAP_OTA, FIELD_TYPE_STRUCT, FIELD_LOC_PERIODIC, offsetof(service_payload_t, ota_status), 0},
    // {"alert", CAP_ALERT, FIELD_TYPE_STRUCT, FIELD_LOC_SPORADIC, offsetof(sporadic_service_payload_t, data.alert), 0},
    // {"ota_event", CAP_OTA, FIELD_TYPE_STRUCT, FIELD_LOC_SPORADIC, offsetof(sporadic_service_payload_t, data.ota), 0},
};

const field_lookup_t *nodeio_find_services_lut_field_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < (sizeof(services_unified_lut) / sizeof(services_unified_lut[0])); ++i)
    {
        if (strcmp(services_unified_lut[i].name, name) == 0)
            return &services_unified_lut[i];
    }
    return NULL;
}

const field_lookup_t *nodeio_get_services_lut(void)
{
    return services_unified_lut;
}

size_t nodeio_get_services_lut_count(void)
{
    return sizeof(services_unified_lut) / sizeof(services_unified_lut[0]);
}

/*
 * nodeio_set_service_lut_struct_from_json
 * High-level parser that maps a cJSON object representing a service payload
 * into the correct struct inside protocol_msg_t. This helper understands the
 * small set of structured services currently used (diagnostics, ota_status,
 * alert, ota_event). It validates types and enforces periodic vs sporadic
 * constraints declared in the LUT.
 */
nio_s_err_t nodeio_set_service_lut_struct_from_json(protocol_msg_t *p_msg, size_t payload_index, const char *service_name, cJSON *service_obj)
{
    if (!p_msg || !service_name || !service_obj)
        return NIO_S_BAD_ARG;

    const field_lookup_t *fld = nodeio_find_services_lut_field_by_name(service_name);
    if (!fld)
    {
        ESP_LOGW(TAG, "Service '%s' not found in LUT", service_name);
        return NIO_S_NOT_FOUND;
    }

/* Helper macro to choose target base depending on field location */
#define TARGET_PERIODIC (&p_msg->payload.periodic_data[payload_index].datafields.service)
#define TARGET_SPORADIC (&p_msg->payload.sporadic_data.datafields.service)

    // Handle diagnostics
    if (strcmp(service_name, "diagnostics") == 0)
    {
        cJSON *uptime_item = cJSON_GetObjectItem(service_obj, "uptime_sec");
        cJSON *free_heap_item = cJSON_GetObjectItem(service_obj, "free_heap");
        cJSON *rssi_item = cJSON_GetObjectItem(service_obj, "rssi");
        cJSON *error_code_item = cJSON_GetObjectItem(service_obj, "error_code");
        if (uptime_item && free_heap_item && rssi_item && error_code_item && cJSON_IsNumber(uptime_item) && cJSON_IsNumber(free_heap_item) && cJSON_IsNumber(rssi_item) && cJSON_IsNumber(error_code_item))
        {
            /* diagnostics is a periodic-only service in the LUT; reject sporadic target */
            if (fld->loc != FIELD_LOC_PERIODIC)
                return NIO_S_BAD_ARG;
            if (payload_index >= PROTOCOL_MAX_PAYLOAD_COUNT)
                return NIO_S_OOB;
            TARGET_PERIODIC->diagnostics.uptime_sec = uptime_item->valueint;
            TARGET_PERIODIC->diagnostics.free_heap = free_heap_item->valueint;
            TARGET_PERIODIC->diagnostics.rssi = rssi_item->valueint;
            TARGET_PERIODIC->diagnostics.error_code = error_code_item->valueint;
            p_msg->payload.periodic_data[payload_index].current_cap_mask |= CAP_DIAG;
            return NIO_S_OK;
        }
        return NIO_S_BAD_ARG;
    }

    // Handle ota_status
    if (strcmp(service_name, "ota_status") == 0 || strcmp(service_name, "ota") == 0)
    {
        cJSON *status_code_item = cJSON_GetObjectItem(service_obj, "status_code");
        cJSON *message_item = cJSON_GetObjectItem(service_obj, "message");
        if (status_code_item && cJSON_IsNumber(status_code_item) && message_item && cJSON_IsString(message_item))
        {
            /* ota_status is periodic in the LUT; reject sporadic target for this name */
            if (fld->loc != FIELD_LOC_PERIODIC)
                return NIO_S_BAD_ARG;
            if (payload_index >= PROTOCOL_MAX_PAYLOAD_COUNT)
                return NIO_S_OOB;
            TARGET_PERIODIC->ota_status.status_code = status_code_item->valueint;
            strncpy(TARGET_PERIODIC->ota_status.message, message_item->valuestring, sizeof(TARGET_PERIODIC->ota_status.message) - 1);
            TARGET_PERIODIC->ota_status.message[sizeof(TARGET_PERIODIC->ota_status.message) - 1] = '\0';
            p_msg->payload.periodic_data[payload_index].current_cap_mask |= CAP_OTA;
            return NIO_S_OK;
        }
        return NIO_S_BAD_ARG;
    }

    /* Sporadic-only services: alert, ota_event */
    if (fld->loc == FIELD_LOC_SPORADIC)
    {
        if (strcmp(service_name, "alert") == 0)
        {
            cJSON *code_item = cJSON_GetObjectItem(service_obj, "alert_code");
            cJSON *msg_item = cJSON_GetObjectItem(service_obj, "alert_message");
            if (code_item && msg_item && cJSON_IsNumber(code_item) && cJSON_IsString(msg_item))
            {
                TARGET_SPORADIC->data.alert.alert_code = code_item->valueint;
                strncpy(TARGET_SPORADIC->data.alert.alert_message, msg_item->valuestring, sizeof(TARGET_SPORADIC->data.alert.alert_message) - 1);
                TARGET_SPORADIC->data.alert.alert_message[sizeof(TARGET_SPORADIC->data.alert.alert_message) - 1] = '\0';
                p_msg->payload.sporadic_data.current_cap_mask |= CAP_ALERT;
                return NIO_S_OK;
            }
            return NIO_S_BAD_ARG;
        }
        else if (strcmp(service_name, "ota_event") == 0)
        {
            cJSON *code_item = cJSON_GetObjectItem(service_obj, "ota_status_code");
            cJSON *msg_item = cJSON_GetObjectItem(service_obj, "ota_message");
            if (code_item && msg_item && cJSON_IsNumber(code_item) && cJSON_IsString(msg_item))
            {
                TARGET_SPORADIC->data.ota.ota_status_code = code_item->valueint;
                strncpy(TARGET_SPORADIC->data.ota.ota_message, msg_item->valuestring, sizeof(TARGET_SPORADIC->data.ota.ota_message) - 1);
                TARGET_SPORADIC->data.ota.ota_message[sizeof(TARGET_SPORADIC->data.ota.ota_message) - 1] = '\0';
                p_msg->payload.sporadic_data.current_cap_mask |= CAP_OTA;
                return NIO_S_OK;
            }
            return NIO_S_BAD_ARG;
        }
    }

    ESP_LOGW(TAG, "Service struct '%s' not handled by generic setter", service_name);
    return NIO_S_NOT_FOUND;

#undef TARGET_PERIODIC
#undef TARGET_SPORADIC
}

/*
 * nodeio_serialize_service_lut
 * Small helper to turn a known service struct into a compact JSON-like string
 * for UI/reporting. Keep the output short to avoid wasting RAM on the hub.
 */
nio_s_err_t nodeio_serialize_service_lut(const protocol_msg_t *p_msg, size_t payload_index, const char *service_name, char *dst, size_t dst_len)
{
    if (!p_msg || !service_name || !dst)
        return NIO_S_BAD_ARG;
    if (strcmp(service_name, "diagnostics") == 0)
    {
        if (payload_index >= PROTOCOL_MAX_PAYLOAD_COUNT)
            return NIO_S_OOB;
        int err = snprintf(dst, dst_len, "{\"uptime_sec\":%" PRIu32 ",\"free_heap\":%" PRIu32 ",\"rssi\":%d,\"error_code\":%d}",
                           p_msg->payload.periodic_data[payload_index].datafields.service.diagnostics.uptime_sec,
                           p_msg->payload.periodic_data[payload_index].datafields.service.diagnostics.free_heap,
                           (int)p_msg->payload.periodic_data[payload_index].datafields.service.diagnostics.rssi,
                           (int)p_msg->payload.periodic_data[payload_index].datafields.service.diagnostics.error_code);
        return (err < 0) ? NIO_S_BAD_ARG : NIO_S_OK;
    }
    else if (strcmp(service_name, "ota") == 0 || strcmp(service_name, "ota_status") == 0)
    {
        if (payload_index >= PROTOCOL_MAX_PAYLOAD_COUNT)
            return NIO_S_OOB;
        int err = snprintf(dst, dst_len, "{\"status_code\":%d,\"message\":\"%s\"}", p_msg->payload.periodic_data[payload_index].datafields.service.ota_status.status_code,
                           p_msg->payload.periodic_data[payload_index].datafields.service.ota_status.message);
        return (err < 0) ? NIO_S_BAD_ARG : NIO_S_OK;
    }
    return NIO_S_NOT_FOUND;
}
