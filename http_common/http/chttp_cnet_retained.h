#ifndef CHTTP_CNET_RETAINED_H
#define CHTTP_CNET_RETAINED_H

#include <cnet/cnet.h>
#include <salts/error_codes.h>
#include <salts_buffer.h>

#include <stddef.h>
#include <stdlib.h>

static inline int chttp_cnet_retained_idle(const mem_buffer_t *buffer) {
  return buffer == NULL || mem_buffer_ref_count(buffer) == 1u;
}

/*
 * Binds one persistent external wrapper to caller-owned storage.
 * The raw allocation remains caller-owned; the wrapper free callback is NULL.
 * Rebind is legal only when CNet holds no retained reference.
 */
static inline int chttp_cnet_retained_bind(mem_buffer_t **slot, void *data, size_t capacity) {
  mem_buffer_t *buffer;
  if (slot == NULL || data == NULL || capacity == 0u) return SALTS_EINVAL;
  buffer = *slot;
  if (buffer == NULL) {
    buffer = mem_wrap_external(data, capacity, NULL, NULL);
    if (buffer == NULL) return SALTS_ENOMEM;
    *slot = buffer;
    mem_set_used(buffer, 0u);
    return SALTS_OK;
  }
  if (!mem_is_external(buffer) || mem_buffer_ref_count(buffer) != 1u) return SALTS_EBUSY;
  buffer->data = (char *)data;
  buffer->capacity = capacity;
  mem_set_used(buffer, 0u);
  return SALTS_OK;
}

static inline int chttp_cnet_retained_prepare(mem_buffer_t *buffer, const void *data,
                                               size_t capacity, size_t used) {
  if (buffer == NULL || data == NULL || capacity == 0u || used == 0u || used > capacity)
    return SALTS_EINVAL;
  if (!mem_is_external(buffer) || mem_buffer_ref_count(buffer) != 1u ||
      mem_buffer_const_data(buffer) != data || mem_buffer_capacity(buffer) != capacity)
    return SALTS_EBUSY;
  mem_set_used(buffer, used);
  return SALTS_OK;
}

static inline int chttp_cnet_retained_send(cnet_client *client, cnet_connection connection,
                                            mem_buffer_t *buffer, const void *data,
                                            size_t capacity, size_t used, int close_after_send) {
  int status = chttp_cnet_retained_prepare(buffer, data, capacity, used);
  if (status != SALTS_OK) return status;
  return close_after_send ? cnet_send_buffer_and_close(client, connection, buffer)
                          : cnet_send_buffer(client, connection, buffer);
}

static inline int chttp_cnet_retained_release(mem_buffer_t **slot) {
  mem_buffer_t *buffer;
  if (slot == NULL) return SALTS_EINVAL;
  buffer = *slot;
  if (buffer == NULL) return SALTS_OK;
  if (mem_buffer_ref_count(buffer) != 1u) return SALTS_EBUSY;
  *slot = NULL;
  mem_buffer_release(buffer);
  return SALTS_OK;
}

static inline void chttp_cnet_owned_free(void *data, void *user) {
  (void)user;
  free(data);
}

/*
 * Converts one malloc-owned send into a retained CNet ownership transfer.
 * On wrapper creation success the raw pointer belongs to the wrapper. The
 * caller must clear its raw pointer regardless of CNet admission result.
 */
static inline int chttp_cnet_send_owned_malloc(cnet_client *client, cnet_connection connection,
                                                void *data, size_t size) {
  mem_buffer_t *buffer;
  int status;
  if (client == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  buffer = mem_wrap_external(data, size, chttp_cnet_owned_free, NULL);
  if (buffer == NULL) return SALTS_ENOMEM;
  status = cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

#endif /* CHTTP_CNET_RETAINED_H */
