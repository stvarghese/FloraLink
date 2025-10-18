// Helper APIs for unified service table access
#pragma once

#include "nodeio_lut.h"
#include <stdint.h>

// Forward-declare cJSON to avoid forcing this header to include cJSON.h
typedef struct cJSON cJSON;

/*
 * Service LUT helpers
 * - Provide a compact mapping from service JSON keys to locations inside
 *   protocol_msg_t. Use nodeio_set_service_lut_struct_from_json to parse
 *   a cJSON object into the correct struct and nodeio_serialize_service_lut
 *   to produce a compact representation for UI.
 * - When adding a new structured service, update the services_unified_lut in
 *   nodeio_services.c and implement parsing/serialization in this module.
 */

typedef enum
{
    NIO_S_OK = 0,
    NIO_S_NOT_FOUND = -1,
    NIO_S_BAD_ARG = -2,
    NIO_S_OOB = -3
} nio_s_err_t;

// Lookup a service-LUT entry by name (returns pointer to field_lookup_t or NULL)
const field_lookup_t *nodeio_find_services_lut_field_by_name(const char *name);

// Parse and set a service struct from cJSON object into the provided protocol_msg_t
// This is intended for FIELD_TYPE_STRUCT entries. The implementation will inspect the service name
// and handle parsing for known service structs (diagnostics, ota_status, alert, etc.).
nio_s_err_t nodeio_set_service_lut_struct_from_json(protocol_msg_t *p_msg, size_t payload_index, const char *service_name, cJSON *service_obj);

// Serialize a service struct into a small JSON-like buffer (for UI/reporting). dst must be large enough.
nio_s_err_t nodeio_serialize_service_lut(const protocol_msg_t *p_msg, size_t payload_index, const char *service_name, char *dst, size_t dst_len);

// Accessors to inspect the LUT pointer and count (prefer using APIs)
const field_lookup_t *nodeio_get_services_lut(void);
size_t nodeio_get_services_lut_count(void);
