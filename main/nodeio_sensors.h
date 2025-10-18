// Helper APIs for unified sensor table access
#pragma once

#include "nodeio_lut.h"
#include <stdint.h>

// Result codes
typedef enum
{
    NIO_OK = 0,
    NIO_ERR_NOT_FOUND = -1,
    NIO_ERR_BAD_ARG = -2,
    NIO_ERR_OOB = -3
} nio_err_t;

/*
 * The unified LUT APIs provide a compact and efficient mapping from JSON
 * keys to in-memory offsets inside protocol_msg_t. This allows the parser to
 * update fields without large switch statements or duplicated logic.
 *
 * Safety guidance:
 * - Prefer nodeio_set_sensor_lut_field/nodeio_get_sensor_lut_field for all
 *   runtime reads/writes. Do not compute offsets or write into message
 *   structs directly elsewhere in the codebase.
 * - When adding or changing fields in the payload structs, update the LUT
 *   entries here and in the corresponding 'nodeio_services.c' file.
 * - Keep JSON key names stable; changing a name requires coordinated
 *   updates to node firmware and any tests.
 */

// Lookup a sensors-LUT field by JSON key name. Returns pointer to field_lookup_t or NULL if not found.
const field_lookup_t *nodeio_find_sensors_lut_field_by_name(const char *name);

// Safely set a sensor field found in sensors LUT by name into a protocol_msg_t's periodic or sporadic storage.
// - p_msg: target message (must be non-NULL)
// - payload_index: index into periodic_data[] for periodic fields; ignored for sporadic fields
// - field_name: JSON key name (e.g. "temperature" or "door_state")
// - index: for array-valued fields (use 0 for scalars or first element)
// - src: pointer to source data (value) interpreted according to field's type
// Returns NIO_OK on success or negative error code.
nio_err_t nodeio_set_sensor_lut_field(protocol_msg_t *p_msg, size_t payload_index, const char *field_name, size_t index, const void *src);

// Safely get a sensor field into dst. dst must point to a buffer large enough for the field type.
nio_err_t nodeio_get_sensor_lut_field(const protocol_msg_t *p_msg, size_t payload_index, const char *field_name, size_t index, void *dst, size_t dst_size);

// Accessors to inspect the LUT pointer and count (prefer using set/get APIs)
const field_lookup_t *nodeio_get_sensors_lut(void);
size_t nodeio_get_sensors_lut_count(void);
