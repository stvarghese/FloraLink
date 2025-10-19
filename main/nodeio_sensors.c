#include "nodeio_sensors.h"
#include "nodeio.h" // for protocol_msg_t and other externs
#include "nodeioprotocol.h"
// LUT is private to this module and describes how JSON keys map to in-memory
// fields inside protocol messages. This compact table enables very fast
// lookups and zero-copy-ish writes into the packed message structures, but
// it is intentionally low-level — maintainers must be careful when editing it.
#include <string.h>
#include <stddef.h>
#include "esp_log.h"

static const char *TAG = "nodeio_sensors";

/*
 * sensors_unified_lut
 * - Central lookup table mapping JSON key names (e.g. "temperature") to:
 *   - capability flag (which bit in capability_mask enables it)
 *   - value type (float, uint8, bool, array, struct)
 *   - storage location (periodic array slot vs sporadic area)
 *   - byte offset within the target struct where the field begins
 *   - element count for arrays
 *
 * IMPORTANT MAINTENANCE NOTES:
 * - The offsets are computed with offsetof() and must match the target
 *   structs declared elsewhere (sensor_payload_t, sporadic_sensor_payload_t).
 *   If you change field ordering in those structs, update the LUT entries.
 * - FIELD_LOC_PERIODIC entries are written into per-message periodic slots
 *   (p_msg->payload.periodic_data[n].datafields). FIELD_LOC_SPORADIC entries
 *   go into p_msg->payload.sporadic_data.datafields. The helpers avoid
 *   duplicating the location logic by computing a base pointer for writes.
 * - The LUT is intentionally read-only at runtime; extend it only at build
 *   time and keep names stable because they appear in node JSON messages.
 */
static const field_lookup_t sensors_unified_lut[] = {
    {"temperature", CAP_TEMP, FIELD_TYPE_FLOAT, FIELD_LOC_PERIODIC, offsetof(sensor_payload_t, temp), 0},
    {"moisture", CAP_MOISTURE, FIELD_TYPE_FLOAT, FIELD_LOC_PERIODIC, offsetof(sensor_payload_t, moisture), 0},
    {"humidity", CAP_HUMIDITY, FIELD_TYPE_FLOAT, FIELD_LOC_PERIODIC, offsetof(sensor_payload_t, humidity), 0},
    {"distance", CAP_DISTANCE, FIELD_TYPE_FLOAT, FIELD_LOC_PERIODIC, offsetof(sensor_payload_t, distance), 0},
    {"light", CAP_LIGHTSENSE, FIELD_TYPE_FLOAT, FIELD_LOC_PERIODIC, offsetof(sensor_payload_t, light), 0},
    /* Sporadic fields: doorsense (door_state array) is stored in sporadic area */
    {"doorsense", CAP_DOORSENSE, FIELD_TYPE_UINT8_ARRAY, FIELD_LOC_SPORADIC, offsetof(sporadic_sensor_payload_t, data.door.door_state[0]), NUM_DOOR_SENSORS},
    // {"button_pressed", CAP_ALERT, FIELD_TYPE_BOOL, FIELD_LOC_SPORADIC, offsetof(sporadic_sensor_payload_t, data.button.button_pressed), 0},
    // {"motion_detected", CAP_ALERT, FIELD_TYPE_BOOL, FIELD_LOC_SPORADIC, offsetof(sporadic_sensor_payload_t, data.motion.motion_detected), 0},
};

/* Public accessors - keep these simple so callers don't need to know the LUT
 * layout. Prefer the setter/getter helper APIs below rather than directly
 * indexing the table. */
const field_lookup_t *nodeio_get_sensors_lut(void)
{
    return sensors_unified_lut;
}

size_t nodeio_get_sensors_lut_count(void)
{
    return sizeof(sensors_unified_lut) / sizeof(sensors_unified_lut[0]);
}

/* Find an entry by JSON key name. Returns NULL if not found. This helper is
 * used by higher-level parsers that translate JSON keys into writes into the
 * packed protocol message structures. */
const field_lookup_t *nodeio_find_sensors_lut_field_by_name(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < nodeio_get_sensors_lut_count(); ++i)
    {
        if (strcmp(sensors_unified_lut[i].name, name) == 0)
            return &sensors_unified_lut[i];
    }
    return NULL;
}

static const field_lookup_t *find_field(const char *name)
{
    return (const field_lookup_t *)nodeio_find_sensors_lut_field_by_name(name);
}

/*
 * get_field_base_ptr
 * Returns a pointer to the base struct that contains the field described by
 * 'fld' inside 'p_msg'. For periodic fields this points at
 * p_msg->payload.periodic_data[payload_index].datafields; for sporadic fields
 * it points at p_msg->payload.sporadic_data.datafields. Callers add fld->offset
 * to position to the exact byte location to read/write.
 */
static void *get_field_base_ptr(protocol_msg_t *p_msg, const field_lookup_t *fld, size_t payload_index)
{
    if (!p_msg || !fld)
        return NULL;
    if (fld->loc == FIELD_LOC_PERIODIC)
    {
        /* periodic data lives in payload.periodic_data[payload_index].datafields */
        if (payload_index >= PROTOCOL_MAX_PAYLOAD_COUNT)
            return NULL;
        return (void *)&p_msg->payload.periodic_data[payload_index].datafields;
    }
    else
    {
        return (void *)&p_msg->payload.sporadic_data.datafields;
    }
}

/*
 * nodeio_set_sensor_lut_field
 * A safe helper that writes a single sensor field into either the periodic
 * slot (payload_index) or the sporadic area based on the LUT entry. It
 * validates element indices, performs sized copies, and updates the
 * corresponding current_cap_mask so downstream code knows which fields are
 * present in the message.
 */
nio_err_t nodeio_set_sensor_lut_field(protocol_msg_t *p_msg, size_t payload_index, const char *field_name, size_t index, const void *src)
{
    if (!p_msg || !field_name || !src)
        return NIO_ERR_BAD_ARG;
    const field_lookup_t *fld = find_field(field_name);
    if (!fld)
        return NIO_ERR_NOT_FOUND;

    void *base = get_field_base_ptr(p_msg, fld, payload_index);
    if (!base)
        return NIO_ERR_BAD_ARG;

    uint8_t *target = (uint8_t *)base + fld->offset;

    switch (fld->ftype)
    {
    case FIELD_TYPE_FLOAT:
        if (fld->elem_count != 0 && index >= fld->elem_count)
            return NIO_ERR_OOB;
        {
            float val = *(const float *)src;
            /* copy the float value into the target location */
            memcpy(target, &val, sizeof(val));
        }
        break;
    case FIELD_TYPE_UINT8:
        if (fld->elem_count != 0 && index >= fld->elem_count)
            return NIO_ERR_OOB;
        {
            uint8_t val = *(const uint8_t *)src;
            memcpy(target + index * sizeof(uint8_t), &val, sizeof(uint8_t));
        }
        break;
    case FIELD_TYPE_BOOL:
        if (fld->elem_count != 0 && index >= fld->elem_count)
            return NIO_ERR_OOB;
        {
            uint8_t val = (*(const uint8_t *)src) ? 1 : 0;
            memcpy(target + index * sizeof(uint8_t), &val, sizeof(uint8_t));
        }
        break;
    case FIELD_TYPE_UINT8_ARRAY:
        if (index >= fld->elem_count)
            return NIO_ERR_OOB;
        /* src points to single element value (uint8_t) */
        {
            uint8_t val = *(const uint8_t *)src;
            memcpy(target + index * sizeof(uint8_t), &val, sizeof(uint8_t));
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported field type for set: %d", fld->ftype);
        return NIO_ERR_BAD_ARG;
    }

    /* Update capability mask on message where appropriate so consumers know
       which fields are populated. */
    if (fld->loc == FIELD_LOC_PERIODIC)
    {
        p_msg->payload.periodic_data[payload_index].current_cap_mask |= fld->cap;
    }
    else
    {
        p_msg->payload.sporadic_data.current_cap_mask |= fld->cap;
    }

    return NIO_OK;
}

/*
 * nodeio_get_sensor_lut_field
 * Balanced getter that reads a field described in LUT out of a protocol_msg_t
 * into a caller-provided buffer. It validates sizes and indices before copying.
 */
nio_err_t nodeio_get_sensor_lut_field(const protocol_msg_t *p_msg, size_t payload_index, const char *field_name, size_t index, void *dst, size_t dst_size)
{
    if (!p_msg || !field_name || !dst)
        return NIO_ERR_BAD_ARG;
    const field_lookup_t *fld = find_field(field_name);
    if (!fld)
        return NIO_ERR_NOT_FOUND;
    const void *base = (fld->loc == FIELD_LOC_PERIODIC) ? (const void *)&p_msg->payload.periodic_data[payload_index].datafields : (const void *)&p_msg->payload.sporadic_data.datafields;
    const uint8_t *src = (const uint8_t *)base + fld->offset;

    switch (fld->ftype)
    {
    case FIELD_TYPE_FLOAT:
        if (dst_size < sizeof(float))
            return NIO_ERR_BAD_ARG;
        memcpy(dst, src, sizeof(float));
        break;
    case FIELD_TYPE_UINT8:
    case FIELD_TYPE_BOOL:
        if (index >= fld->elem_count && fld->elem_count != 0)
            return NIO_ERR_OOB;
        if (dst_size < sizeof(uint8_t))
            return NIO_ERR_BAD_ARG;
        memcpy(dst, src + index * sizeof(uint8_t), sizeof(uint8_t));
        break;
    case FIELD_TYPE_UINT8_ARRAY:
        if (dst_size < fld->elem_count * sizeof(uint8_t))
            return NIO_ERR_BAD_ARG;
        memcpy(dst, src, fld->elem_count * sizeof(uint8_t));
        break;
    default:
        ESP_LOGW(TAG, "Unsupported field type for get: %d", fld->ftype);
        return NIO_ERR_BAD_ARG;
    }
    return NIO_OK;
}
