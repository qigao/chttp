/**
 * @file chttp_h2_hpack.h
 * @brief Private bounded HPACK codec for CHTTP HTTP/2.
 *
 * Migrated from the legacy HTTP repository commit
 * 38f1e389b3f94909db6cb2482a8cbc16522e7e4f and adapted to explicit capacity
 * limits. This header is private and is not installed.
 */

#ifndef CHTTP_H2_HPACK_H
#define CHTTP_H2_HPACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_h2_hpack_s chttp_h2_hpack;

typedef struct chttp_h2_hpack_config {
  size_t max_dynamic_table_bytes;
  size_t max_header_block_bytes;
  size_t max_string_bytes;
} chttp_h2_hpack_config;

typedef struct chttp_h2_hpack_buffer {
  uint8_t *data;
  size_t size;
  size_t capacity;
  size_t max_capacity;
} chttp_h2_hpack_buffer;

typedef struct chttp_h2_hpack_header {
  const char *name;
  size_t name_size;
  const char *value;
  size_t value_size;
} chttp_h2_hpack_header;

typedef int (*chttp_h2_hpack_callback)(void *user, const char *name, size_t name_size,
                                       const char *value, size_t value_size);

int chttp_h2_hpack_buffer_init(chttp_h2_hpack_buffer *buffer, size_t initial_capacity,
                               size_t max_capacity);
void chttp_h2_hpack_buffer_destroy(chttp_h2_hpack_buffer *buffer);
int chttp_h2_hpack_buffer_reserve(chttp_h2_hpack_buffer *buffer, size_t additional_size);
int chttp_h2_hpack_integer_encode(chttp_h2_hpack_buffer *buffer, uint32_t value,
                                  unsigned int prefix_bits);

chttp_h2_hpack *chttp_h2_hpack_create(const chttp_h2_hpack_config *config);
void chttp_h2_hpack_destroy(chttp_h2_hpack *hpack);
int chttp_h2_hpack_encoder_set_max_size(chttp_h2_hpack *hpack, size_t max_size);
int chttp_h2_hpack_decoder_set_max_size(chttp_h2_hpack *hpack, size_t max_size);
int chttp_h2_hpack_decoder_table_size_update(chttp_h2_hpack *hpack, size_t new_size);

int chttp_h2_hpack_encode(chttp_h2_hpack *hpack, chttp_h2_hpack_buffer *output,
                          const chttp_h2_hpack_header *headers, size_t header_count);
int chttp_h2_hpack_decode(chttp_h2_hpack *hpack, const uint8_t *input, size_t input_size,
                          size_t *consumed_size, chttp_h2_hpack_callback callback, void *user,
                          size_t max_header_list_bytes);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_H2_HPACK_H */
