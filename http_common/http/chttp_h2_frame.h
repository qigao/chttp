/**
 * @file chttp_h2_frame.h
 * @brief Private HTTP/2 frame codec derived from RFC 9113 sections 4-6.
 *
 * Migrated from the legacy HTTP repository commit
 * 38f1e389b3f94909db6cb2482a8cbc16522e7e4f and adapted to CHTTP naming and
 * Salts error codes. This header is private and is not installed.
 */

#ifndef CHTTP_H2_FRAME_H
#define CHTTP_H2_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHTTP_H2_FRAME_HEADER_SIZE 9u

typedef enum chttp_h2_frame_type {
  CHTTP_H2_FRAME_DATA = 0,
  CHTTP_H2_FRAME_HEADERS = 1,
  CHTTP_H2_FRAME_PRIORITY = 2,
  CHTTP_H2_FRAME_RST_STREAM = 3,
  CHTTP_H2_FRAME_SETTINGS = 4,
  CHTTP_H2_FRAME_PUSH_PROMISE = 5,
  CHTTP_H2_FRAME_PING = 6,
  CHTTP_H2_FRAME_GOAWAY = 7,
  CHTTP_H2_FRAME_WINDOW_UPDATE = 8,
  CHTTP_H2_FRAME_CONTINUATION = 9
} chttp_h2_frame_type;

#define CHTTP_H2_FLAG_END_STREAM 0x01u
#define CHTTP_H2_FLAG_ACK 0x01u
#define CHTTP_H2_FLAG_END_HEADERS 0x04u
#define CHTTP_H2_FLAG_PADDED 0x08u
#define CHTTP_H2_FLAG_PRIORITY 0x20u

typedef struct chttp_h2_frame_header {
  uint32_t length;
  uint8_t type;
  uint8_t flags;
  uint32_t stream_id;
} chttp_h2_frame_header;

int chttp_h2_frame_header_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                 uint32_t length, uint8_t type, uint8_t flags, uint32_t stream_id);
int chttp_h2_frame_header_decode(const uint8_t *input, size_t input_size, size_t *consumed_size,
                                 chttp_h2_frame_header *header, uint32_t max_frame_size);

int chttp_h2_frame_data_payload(const uint8_t *payload, size_t payload_size, uint8_t flags,
                                const uint8_t **data, size_t *data_size);
int chttp_h2_frame_headers_payload(const uint8_t *payload, size_t payload_size, uint8_t flags,
                                   const uint8_t **block, size_t *block_size, int *has_priority,
                                   uint32_t *dependency_stream_id, uint32_t *weight);

int chttp_h2_frame_settings_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                   const uint32_t *identifiers, const uint32_t *values,
                                   size_t count);
int chttp_h2_frame_settings_parse(const uint8_t *payload, size_t payload_size,
                                  uint32_t *identifiers, uint32_t *values, size_t *count,
                                  size_t max_count);
int chttp_h2_frame_rst_encode(uint8_t *output, size_t capacity, size_t *output_size,
                              uint32_t error_code);
int chttp_h2_frame_goaway_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                 uint32_t last_stream_id, uint32_t error_code);
int chttp_h2_frame_goaway_parse(const uint8_t *payload, size_t payload_size,
                                uint32_t *last_stream_id, uint32_t *error_code);
int chttp_h2_frame_ping_encode(uint8_t *output, size_t capacity, size_t *output_size,
                               const uint8_t *opaque_bytes);
int chttp_h2_frame_window_update_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                        uint32_t increment);
int chttp_h2_frame_priority_encode(uint8_t *output, size_t capacity, size_t *output_size,
                                   int exclusive, uint32_t dependency_stream_id, uint32_t weight);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_H2_FRAME_H */
