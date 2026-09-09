#include "chttp_h2_frame.h"

#include <salts/error_codes.h>

#include <limits.h>
#include <string.h>

static uint32_t chttp_h2_read_u24(const uint8_t *input) {
  return ((uint32_t)input[0] << 16) | ((uint32_t)input[1] << 8) | (uint32_t)input[2];
}

static uint32_t chttp_h2_read_u32(const uint8_t *input) {
  return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) | ((uint32_t)input[2] << 8) |
         (uint32_t)input[3];
}

static void chttp_h2_write_u24(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)(value >> 16);
  output[1] = (uint8_t)(value >> 8);
  output[2] = (uint8_t)value;
}

static void chttp_h2_write_u32(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)(value >> 24);
  output[1] = (uint8_t)(value >> 16);
  output[2] = (uint8_t)(value >> 8);
  output[3] = (uint8_t)value;
}

int chttp_h2_frame_header_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                 uint32_t length, uint8_t type, uint8_t flags, uint32_t stream_id) {
  if (output == NULL || output_size == NULL) return SALTS_EINVAL;
  if (capacity < CHTTP_H2_FRAME_HEADER_SIZE) return SALTS_EMSGSIZE;
  if (length > 0x00ffffffu || (stream_id & 0x80000000u) != 0u) return SALTS_EINVAL;

  chttp_h2_write_u24(output, length);
  output[3] = type;
  output[4] = flags;
  chttp_h2_write_u32(output + 5u, stream_id);
  *output_size = CHTTP_H2_FRAME_HEADER_SIZE;
  return SALTS_OK;
}

int chttp_h2_frame_header_decode(const uint8_t *input, size_t input_size, size_t *consumed_size,
                                 chttp_h2_frame_header *header, uint32_t max_frame_size) {
  uint32_t length;
  if (input == NULL || consumed_size == NULL || header == NULL) return SALTS_EINVAL;
  if (input_size < CHTTP_H2_FRAME_HEADER_SIZE) return SALTS_EMSGSIZE;

  length = chttp_h2_read_u24(input);
  if (length > max_frame_size) return SALTS_EMSGSIZE;
  header->length = length;
  header->type = input[3];
  header->flags = input[4];
  header->stream_id = chttp_h2_read_u32(input + 5u) & 0x7fffffffu;
  *consumed_size = CHTTP_H2_FRAME_HEADER_SIZE;
  return SALTS_OK;
}

int chttp_h2_frame_data_payload(const uint8_t *payload, size_t payload_size, uint8_t flags,
                                const uint8_t **data, size_t *data_size) {
  size_t padding_size;
  if (payload == NULL || data == NULL || data_size == NULL) return SALTS_EINVAL;
  if ((flags & CHTTP_H2_FLAG_PADDED) == 0u) {
    *data = payload;
    *data_size = payload_size;
    return SALTS_OK;
  }
  if (payload_size == 0u) return SALTS_EPROTO;
  padding_size = payload[0];
  if (padding_size > payload_size - 1u) return SALTS_EPROTO;
  *data = payload + 1u;
  *data_size = payload_size - 1u - padding_size;
  return SALTS_OK;
}

int chttp_h2_frame_headers_payload(const uint8_t *payload, size_t payload_size, uint8_t flags,
                                   const uint8_t **block, size_t *block_size, int *has_priority,
                                   uint32_t *dependency_stream_id, uint32_t *weight) {
  size_t offset = 0u;
  size_t padding_size = 0u;
  if (payload == NULL || block == NULL || block_size == NULL) return SALTS_EINVAL;

  if ((flags & CHTTP_H2_FLAG_PADDED) != 0u) {
    if (payload_size == 0u) return SALTS_EPROTO;
    padding_size = payload[0];
    offset = 1u;
    if (padding_size > payload_size - offset) return SALTS_EPROTO;
  }

  if ((flags & CHTTP_H2_FLAG_PRIORITY) != 0u) {
    if (payload_size - offset - padding_size < 5u) return SALTS_EPROTO;
    if (dependency_stream_id != NULL)
      *dependency_stream_id = chttp_h2_read_u32(payload + offset) & 0x7fffffffu;
    if (weight != NULL) *weight = (uint32_t)payload[offset + 4u] + 1u;
    if (has_priority != NULL) *has_priority = 1;
    offset += 5u;
  } else if (has_priority != NULL) {
    *has_priority = 0;
  }

  *block = payload + offset;
  *block_size = payload_size - offset - padding_size;
  return SALTS_OK;
}

int chttp_h2_frame_settings_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                   const uint32_t *identifiers, const uint32_t *values,
                                   size_t count) {
  size_t index;
  size_t required_size;
  if (output == NULL || output_size == NULL ||
      (count != 0u && (identifiers == NULL || values == NULL)))
    return SALTS_EINVAL;
  if (count > SIZE_MAX / 6u) return SALTS_EMSGSIZE;
  required_size = count * 6u;
  if (capacity < required_size) return SALTS_EMSGSIZE;

  for (index = 0u; index < count; ++index) {
    if (identifiers[index] > UINT16_MAX) return SALTS_EINVAL;
    output[index * 6u] = (uint8_t)(identifiers[index] >> 8);
    output[index * 6u + 1u] = (uint8_t)identifiers[index];
    chttp_h2_write_u32(output + index * 6u + 2u, values[index]);
  }
  *output_size = required_size;
  return SALTS_OK;
}

int chttp_h2_frame_settings_parse(const uint8_t *payload, size_t payload_size,
                                  uint32_t *identifiers, uint32_t *values, size_t *count,
                                  size_t max_count) {
  size_t index;
  size_t parsed_count;
  if (payload == NULL || count == NULL) return SALTS_EINVAL;
  if (payload_size % 6u != 0u) return SALTS_EPROTO;
  parsed_count = payload_size / 6u;
  if (parsed_count > max_count) return SALTS_EMSGSIZE;

  for (index = 0u; index < parsed_count; ++index) {
    if (identifiers != NULL)
      identifiers[index] =
          ((uint32_t)payload[index * 6u] << 8) | (uint32_t)payload[index * 6u + 1u];
    if (values != NULL) values[index] = chttp_h2_read_u32(payload + index * 6u + 2u);
  }
  *count = parsed_count;
  return SALTS_OK;
}

int chttp_h2_frame_rst_encode(uint8_t *output, size_t capacity, size_t *output_size,
                              uint32_t error_code) {
  if (output == NULL || output_size == NULL) return SALTS_EINVAL;
  if (capacity < 4u) return SALTS_EMSGSIZE;
  chttp_h2_write_u32(output, error_code);
  *output_size = 4u;
  return SALTS_OK;
}

int chttp_h2_frame_goaway_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                 uint32_t last_stream_id, uint32_t error_code) {
  if (output == NULL || output_size == NULL) return SALTS_EINVAL;
  if (capacity < 8u) return SALTS_EMSGSIZE;
  if ((last_stream_id & 0x80000000u) != 0u) return SALTS_EINVAL;
  chttp_h2_write_u32(output, last_stream_id);
  chttp_h2_write_u32(output + 4u, error_code);
  *output_size = 8u;
  return SALTS_OK;
}

int chttp_h2_frame_goaway_parse(const uint8_t *payload, size_t payload_size,
                                uint32_t *last_stream_id, uint32_t *error_code) {
  if (payload == NULL || last_stream_id == NULL || error_code == NULL) return SALTS_EINVAL;
  if (payload_size < 8u) return SALTS_EMSGSIZE;
  *last_stream_id = chttp_h2_read_u32(payload) & 0x7fffffffu;
  *error_code = chttp_h2_read_u32(payload + 4u);
  return SALTS_OK;
}

int chttp_h2_frame_ping_encode(uint8_t *output, size_t capacity, size_t *output_size,
                               const uint8_t *opaque_bytes) {
  if (output == NULL || output_size == NULL || opaque_bytes == NULL) return SALTS_EINVAL;
  if (capacity < 8u) return SALTS_EMSGSIZE;
  memcpy(output, opaque_bytes, 8u);
  *output_size = 8u;
  return SALTS_OK;
}

int chttp_h2_frame_window_update_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                        uint32_t increment) {
  if (output == NULL || output_size == NULL || increment == 0u || (increment & 0x80000000u) != 0u)
    return SALTS_EINVAL;
  if (capacity < 4u) return SALTS_EMSGSIZE;
  chttp_h2_write_u32(output, increment);
  *output_size = 4u;
  return SALTS_OK;
}

int chttp_h2_frame_priority_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                   int exclusive, uint32_t dependency_stream_id, uint32_t weight) {
  uint32_t dependency;
  if (output == NULL || output_size == NULL || weight == 0u || weight > 256u ||
      (dependency_stream_id & 0x80000000u) != 0u)
    return SALTS_EINVAL;
  if (capacity < 5u) return SALTS_EMSGSIZE;
  dependency = dependency_stream_id | (exclusive != 0 ? 0x80000000u : 0u);
  chttp_h2_write_u32(output, dependency);
  output[4] = (uint8_t)(weight - 1u);
  *output_size = 5u;
  return SALTS_OK;
}
