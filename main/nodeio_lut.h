// Shared LUT metadata types used by nodeio_sensors and nodeio_services
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "nodeioprotocol.h"

/*
 * field_lookup_t
 * - One entry describes a single JSON key and where/how to read/write it
 *   inside protocol_msg_t.
 * - name: JSON key as expected in node messages
 * - cap: capability flag that enables the field on the node
 * - ftype: how to interpret/copy the bytes (float, uint8, struct, etc.)
 * - loc: whether the field is stored in the periodic array or the sporadic area
 * - offset: byte offset from the target base struct to the field (use offsetof())
 * - elem_count: for arrays, number of elements; 0 means scalar/non-array
 *
 * Safety notes:
 * - Offsets must match the in-memory layout of the corresponding structs. If
 *   you change struct layouts, update the LUT entries accordingly.
 * - Prefer the provided set/get helpers rather than computing offsets and
 *   writing directly into protocol_msg_t to avoid subtle packing/alignment bugs.
 */
typedef enum
{
    FIELD_TYPE_FLOAT,
    FIELD_TYPE_UINT8,
    FIELD_TYPE_BOOL,
    FIELD_TYPE_UINT8_ARRAY,
    FIELD_TYPE_STRUCT,
} field_type_t;

typedef enum
{
    FIELD_LOC_PERIODIC,
    FIELD_LOC_SPORADIC,
} field_loc_t;

typedef struct
{
    const char *name;   // JSON/key name
    capability_t cap;   // capability bitmask
    field_type_t ftype; // field data type
    field_loc_t loc;    // where the field is stored (periodic vs sporadic)
    size_t offset;      // byte offset within the referenced struct (first element for arrays)
    size_t elem_count;  // number of elements for arrays (0 or 1 for scalars)
} field_lookup_t;
