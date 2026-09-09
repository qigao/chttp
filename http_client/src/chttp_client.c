#include "chttp_client_internal.h"
#include "chttp_h2_session.h"
#include "chttp_internal.h"
#include "chttp_tls.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct chttp_client_impl chttp_client_impl;

enum {
  CHTTP_STREAM_CHUNK_DEFAULT_BYTES = 64u * 1024u,
  CHTTP_FILE_PROGRESS_POLL_MS = 1u,
  CHTTP_H1_CHUNK_PREFIX_BYTES = 2u * sizeof(size_t) + 4u,
  CHTTP_H1_CHUNK_TRAILER_BYTES = 2u,
  CHTTP_H1_CHUNK_OVERHEAD_BYTES = CHTTP_H1_CHUNK_PREFIX_BYTES + CHTTP_H1_CHUNK_TRAILER_BYTES
};

typedef enum chttp_slot_state {
  CHTTP_SLOT_FREE = 0,
  CHTTP_SLOT_CONNECTING,
  CHTTP_SLOT_BUSY,
  CHTTP_SLOT_IDLE,
  CHTTP_SLOT_CLOSING,
  CHTTP_SLOT_TERMINAL
} chttp_slot_state;

typedef struct chttp_slot {
  chttp_client_impl *client;
  chttp_request public_handle;
  cnet_connection connection;
  chttp_response_parser response_parser;
  chttp_h2_request_state h2_request;
  unsigned char *request_data;
  unsigned char *source_buffer;
  char *connection_uri;
  char *authority;
  chttp_tls_profile_impl *tls_profile;
  size_t request_size;
  size_t source_transferred;
  chttp_body_source body_source;
  chttp_file_transfer *file_transfer;
  chttp_file_sink_transfer *file_sink_transfer;
  chttp_complete_fn on_complete;
  void *user;
  uint32_t generation;
  chttp_protocol protocol;
  chttp_slot_state state;
  bool result_delivered;
  bool cancel_requested;
  bool close_admitted;
  bool close_pending;
  bool receive_armed;
  bool source_enabled;
  bool source_complete;
  bool source_final_pending;
  bool transport_closed;
} chttp_slot;

struct chttp_client_impl {
  cnet_client network;
  cflow_io_file_runtime file_runtime;
  chttp_slot *slots;
  chttp_h2_session *h2_sessions;
  chttp_limits limits;
  chttp_h2_proto_config h2_config;
  size_t request_capacity;
  size_t h2_session_capacity;
  size_t completion_count;
  size_t file_sink_capacity;
  int h2_config_status;
  bool admission_open;
  bool poll_active;
  bool callback_active;
  bool stop_active;
  bool network_stop_started;
  bool stopped;
  bool file_runtime_initialized;
};

static chttp_client_impl *chttp_client_get(chttp_async_client *client) {
  return client != NULL ? (chttp_client_impl *)client->impl : NULL;
}

static cflow_io_native_backend_kind chttp_file_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static void chttp_file_runtime_wake(void *user) {
  chttp_client_impl *impl = (chttp_client_impl *)user;
  if (impl != NULL) (void)cnet_client_wake(&impl->network);
}

static int chttp_file_runtime_ensure(chttp_client_impl *impl) {
  cflow_io_file_runtime_config config;
  size_t command_capacity;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->file_runtime_initialized) return SALTS_OK;
  if (!impl->admission_open || impl->poll_active || impl->callback_active) return SALTS_EBUSY;
  command_capacity = impl->request_capacity <= SIZE_MAX / 2u ? impl->request_capacity * 2u
                                                             : impl->request_capacity;
  config = (cflow_io_file_runtime_config){.backend_kind = chttp_file_backend(),
                                          .file_capacity = impl->request_capacity,
                                          .request_capacity = impl->request_capacity,
                                          .command_capacity = command_capacity,
                                          .completion_batch_capacity = impl->request_capacity,
                                          .wake = chttp_file_runtime_wake,
                                          .wake_user = impl};
  status = cflow_io_file_runtime_init(&impl->file_runtime, &config);
  if (status == SALTS_OK) impl->file_runtime_initialized = true;
  return status;
}

static int chttp_file_runtime_progress(chttp_client_impl *impl) {
  size_t max_steps;
  size_t progressed = 0u;
  if (impl == NULL || !impl->file_runtime_initialized) return SALTS_OK;
  max_steps = impl->request_capacity <= SIZE_MAX / 4u ? impl->request_capacity * 4u : SIZE_MAX;
  if (max_steps == 0u) max_steps = 1u;
  return cflow_io_file_runtime_run_ready(&impl->file_runtime, max_steps, &progressed);
}

static uint32_t chttp_file_runtime_poll_timeout(const chttp_client_impl *impl,
                                                uint32_t requested_ms) {
  cflow_io_file_runtime_stats stats = {0};
  if (impl == NULL || !impl->file_runtime_initialized ||
      !cflow_io_file_runtime_get_stats(&impl->file_runtime, &stats) ||
      stats.operation_slots_in_use == 0u || requested_ms <= CHTTP_FILE_PROGRESS_POLL_MS)
    return requested_ms;
  /* CFlow and CNet own independent native wait domains. The cross-runtime
     wake is the fast path; this bounded wait closes the coalesced-edge window
     without polling when no file operation is live. */
  return CHTTP_FILE_PROGRESS_POLL_MS;
}

int chttp_async_client_file_runtime(chttp_async_client *client,
                                    cflow_io_file_runtime **out_runtime) {
  chttp_client_impl *impl = chttp_client_get(client);
  int status;
  if (impl == NULL || out_runtime == NULL) return SALTS_EINVAL;
  *out_runtime = NULL;
  status = chttp_file_runtime_ensure(impl);
  if (status != SALTS_OK) return status;
  *out_runtime = &impl->file_runtime;
  return SALTS_OK;
}

int chttp_async_client_file_sink_capacity(chttp_async_client *client, size_t *out_capacity) {
  chttp_client_impl *impl = chttp_client_get(client);
  if (impl == NULL || out_capacity == NULL || impl->file_sink_capacity == 0u) return SALTS_EINVAL;
  *out_capacity = impl->file_sink_capacity;
  return SALTS_OK;
}

static uint32_t chttp_next_generation(uint32_t generation) {
  ++generation;
  return generation == 0u ? 1u : generation;
}

static char *chttp_copy_text(const char *text) {
  size_t size;
  char *copy;
  if (text == NULL) return NULL;
  size = strlen(text);
  if (size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, text, size + 1u);
  return copy;
}

static bool chttp_config_valid(const chttp_client_config *config) {
  if (config == NULL || config->network.connection_capacity == 0u ||
      config->request_capacity == 0u || config->request_capacity > UINT32_MAX ||
      config->max_start_line_bytes <= 15u || config->max_header_count < 3u ||
      config->max_header_bytes == 0u || config->max_request_body_bytes == 0u ||
      config->max_response_body_bytes == 0u || config->max_informational_responses == 0u ||
      config->network.max_send_bytes == 0u)
    return false;
  if (config->request_capacity > SIZE_MAX / sizeof(chttp_slot)) return false;
  if (config->network.connection_capacity > SIZE_MAX / sizeof(chttp_h2_session)) return false;
  if (config->max_header_bytes == SIZE_MAX || config->max_start_line_bytes == SIZE_MAX)
    return false;
  if (config->stream_chunk_bytes != 0u &&
      (config->network.max_send_bytes <= CHTTP_H1_CHUNK_OVERHEAD_BYTES ||
       config->stream_chunk_bytes > config->network.max_send_bytes - CHTTP_H1_CHUNK_OVERHEAD_BYTES))
    return false;
  return config->max_header_count <= (SIZE_MAX - config->max_header_bytes - 1u) / 2u;
}

static int chttp_stream_uri_supported(const char *uri, bool has_tls_profile) {
  if (uri == NULL) return SALTS_EINVAL;
  if (strncmp(uri, "tls://", sizeof("tls://") - 1u) == 0) return SALTS_OK;
  if (strncmp(uri, "tcp://", sizeof("tcp://") - 1u) == 0 ||
      strncmp(uri, "pipe://", sizeof("pipe://") - 1u) == 0)
    return has_tls_profile ? SALTS_EINVAL : SALTS_OK;
  return SALTS_ENOTSUP;
}

static int chttp_h2_stream_uri_supported(const char *uri, bool has_tls_profile) {
  if (uri == NULL) return SALTS_EINVAL;
  if (strncmp(uri, "tls://", sizeof("tls://") - 1u) == 0)
    return has_tls_profile ? SALTS_OK : SALTS_EPROTONOSUPPORT;
  if (strncmp(uri, "tcp://", sizeof("tcp://") - 1u) == 0)
    return has_tls_profile ? SALTS_EINVAL : SALTS_OK;
  return SALTS_ENOTSUP;
}

static void chttp_slot_release(chttp_slot *slot) {
  chttp_client_impl *client;
  uint32_t generation;
  if (slot == NULL) return;
  client = slot->client;
  generation = slot->generation;
  if (slot->file_transfer != NULL) chttp_file_transfer_set_ready(slot->file_transfer, NULL, NULL);
  if (slot->file_sink_transfer != NULL)
    chttp_file_sink_transfer_set_ready(slot->file_sink_transfer, NULL, NULL);
  free(slot->request_data);
  free(slot->source_buffer);
  free(slot->authority);
  free(slot->connection_uri);
  chttp_tls_profile_release(slot->tls_profile);
  chttp_response_parser_destroy(&slot->response_parser);
  chttp_h2_request_destroy(&slot->h2_request);
  *slot = (chttp_slot){.client = client, .generation = generation};
}

static chttp_slot *chttp_slot_find_free(chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (impl->slots[index].state == CHTTP_SLOT_FREE) return &impl->slots[index];
  return NULL;
}

/* The scan is O(request_capacity), whose configured hard bound also limits CNet connections. */
static chttp_slot *chttp_slot_find_idle(chttp_client_impl *impl,
                                        const chttp_request_options *options,
                                        const chttp_tls_profile_impl *tls_profile) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index) {
    chttp_slot *slot = &impl->slots[index];
    if (slot->state == CHTTP_SLOT_IDLE && slot->connection_uri != NULL && slot->authority != NULL &&
        strcmp(slot->connection_uri, options->connection_uri) == 0 &&
        strcmp(slot->authority, options->authority) == 0 && slot->tls_profile == tls_profile)
      return slot;
  }
  return NULL;
}

static chttp_slot *chttp_slot_find_any_idle(chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (impl->slots[index].state == CHTTP_SLOT_IDLE) return &impl->slots[index];
  return NULL;
}

static chttp_slot *chttp_slot_find(chttp_client_impl *impl, chttp_request request) {
  chttp_slot *slot;
  if (impl == NULL || request.slot == 0u || request.slot > impl->request_capacity ||
      request.generation == 0u)
    return NULL;
  slot = &impl->slots[request.slot - 1u];
  return slot->state != CHTTP_SLOT_FREE && slot->state != CHTTP_SLOT_TERMINAL &&
                 slot->generation == request.generation
             ? slot
             : NULL;
}

static void chttp_slot_deliver(chttp_slot *slot, const chttp_response_view *response, int status,
                               int native_status, const char *stage) {
  chttp_client_impl *impl;
  chttp_error error;
  const chttp_error *error_view = NULL;
  if (slot == NULL || slot->state == CHTTP_SLOT_FREE || slot->state == CHTTP_SLOT_TERMINAL ||
      slot->result_delivered)
    return;
  impl = slot->client;
  slot->result_delivered = true;
  if (status != SALTS_OK) {
    error = (chttp_error){.status = status, .native_status = native_status, .stage = stage};
    error_view = &error;
    response = NULL;
  }
  impl->callback_active = true;
  slot->on_complete(slot->user, slot->public_handle, response, error_view);
  impl->callback_active = false;
  ++impl->completion_count;
}

static int chttp_slot_try_close(chttp_slot *slot) {
  int status;
  if (slot == NULL || slot->state == CHTTP_SLOT_FREE || slot->state == CHTTP_SLOT_TERMINAL ||
      slot->close_admitted)
    return SALTS_OK;
  status = cnet_close(&slot->client->network, slot->connection);
  if (status == SALTS_OK) {
    slot->close_admitted = true;
    slot->close_pending = false;
    slot->state = CHTTP_SLOT_CLOSING;
    return SALTS_OK;
  }
  if (status == SALTS_EALREADY || status == SALTS_ENOENT || status == SALTS_ESHUTDOWN) {
    slot->close_admitted = true;
    slot->close_pending = false;
    slot->state = CHTTP_SLOT_CLOSING;
    return SALTS_OK;
  }
  slot->close_pending = true;
  slot->state = CHTTP_SLOT_CLOSING;
  return status;
}

static void chttp_slot_fail_and_close(chttp_slot *slot, int status, int native_status,
                                      const char *stage) {
  chttp_slot_deliver(slot, NULL, status, native_status, stage);
  if (slot->transport_closed) {
    slot->state = CHTTP_SLOT_TERMINAL;
    slot->close_pending = false;
    return;
  }
  (void)chttp_slot_try_close(slot);
}

static int chttp_slot_arm_receive(chttp_slot *slot) {
  int status;
  if (slot->receive_armed) return SALTS_OK;
  status = cnet_receive(&slot->client->network, slot->connection, 1u);
  if (status == SALTS_OK) slot->receive_armed = true;
  return status;
}

static void chttp_slot_source_finished(chttp_slot *slot) {
  if (slot == NULL) return;
  if (slot->file_transfer != NULL) chttp_file_transfer_set_ready(slot->file_transfer, NULL, NULL);
  free(slot->source_buffer);
  slot->source_buffer = NULL;
  slot->body_source = (chttp_body_source){0};
  slot->file_transfer = NULL;
  slot->source_enabled = false;
  slot->source_complete = true;
  slot->source_final_pending = false;
}

static int chttp_slot_prepare_streaming(chttp_slot *slot, const chttp_request_options *options,
                                        chttp_file_sink_transfer *file_sink_transfer) {
  int status;
  if (slot == NULL || options == NULL) return SALTS_EINVAL;
  status = chttp_response_parser_init_with_sink(&slot->response_parser, options->method,
                                                &slot->client->limits, options->body_sink);
  if (status != SALTS_OK) return status;
  slot->response_parser.file_sink_transfer = file_sink_transfer;
  slot->source_complete = options->body_source == NULL;
  if (options->body_source == NULL) return SALTS_OK;
  if (slot->client->limits.stream_chunk_bytes == 0u ||
      slot->client->limits.stream_chunk_bytes > SIZE_MAX - CHTTP_H1_CHUNK_OVERHEAD_BYTES) {
    chttp_response_parser_destroy(&slot->response_parser);
    return SALTS_EMSGSIZE;
  }
  slot->source_buffer = (unsigned char *)malloc(slot->client->limits.stream_chunk_bytes +
                                                CHTTP_H1_CHUNK_OVERHEAD_BYTES);
  if (slot->source_buffer == NULL) {
    chttp_response_parser_destroy(&slot->response_parser);
    return SALTS_ENOMEM;
  }
  slot->body_source = *options->body_source;
  slot->source_transferred = 0u;
  slot->source_enabled = true;
  slot->source_final_pending = false;
  return SALTS_OK;
}

static void chttp_slot_complete_response(chttp_slot *slot) {
  const bool keep_alive = slot->response_parser.response.protocol_keep_alive != 0;
  int status;
  chttp_slot_deliver(slot, &slot->response_parser.response, SALTS_OK, 0, NULL);
  if (slot->transport_closed) {
    slot->state = CHTTP_SLOT_TERMINAL;
    slot->close_pending = false;
    return;
  }
  if (keep_alive && slot->source_complete && !slot->cancel_requested &&
      !slot->client->stop_active) {
    chttp_response_parser_destroy(&slot->response_parser);
    slot->on_complete = NULL;
    slot->user = NULL;
    if (slot->file_sink_transfer != NULL)
      chttp_file_sink_transfer_set_ready(slot->file_sink_transfer, NULL, NULL);
    slot->file_sink_transfer = NULL;
    slot->state = CHTTP_SLOT_IDLE;
    status = chttp_slot_arm_receive(slot);
    if (status != SALTS_OK) (void)chttp_slot_try_close(slot);
  } else {
    (void)chttp_slot_try_close(slot);
  }
}

static void chttp_slot_file_sink_ready(void *user) {
  chttp_slot *slot = (chttp_slot *)user;
  chttp_file_sink_result result;
  int native_status = 0;
  int status;
  if (slot == NULL || slot->file_sink_transfer == NULL || slot->result_delivered ||
      slot->cancel_requested || slot->client->stop_active || slot->state != CHTTP_SLOT_BUSY)
    return;
  result = chttp_file_sink_transfer_advance(slot->file_sink_transfer);
  if (result == CHTTP_FILE_SINK_WAIT) return;
  if (result == CHTTP_FILE_SINK_ERROR) {
    status = chttp_file_sink_transfer_status(slot->file_sink_transfer, &native_status);
    chttp_slot_fail_and_close(slot, status == SALTS_OK ? SALTS_EIO : status, native_status,
                              "file-write");
    return;
  }
  if (slot->response_parser.complete) {
    chttp_slot_complete_response(slot);
    return;
  }
  status = chttp_slot_arm_receive(slot);
  if (status != SALTS_OK) chttp_slot_fail_and_close(slot, status, 0, "receive-admission");
}

static void chttp_slot_source_advance(chttp_slot *slot) {
  size_t produced = 0u;
  size_t capacity;
  unsigned char *payload;
  int status;
  if (slot == NULL || !slot->source_enabled || slot->source_complete || slot->result_delivered ||
      slot->cancel_requested || slot->client->stop_active || slot->state != CHTTP_SLOT_BUSY)
    return;
  if (slot->source_final_pending) {
    chttp_slot_source_finished(slot);
    return;
  }
  capacity = slot->client->limits.stream_chunk_bytes;
  if (slot->body_source.content_length_known) {
    const size_t remaining = slot->body_source.content_length - slot->source_transferred;
    if (remaining < capacity) capacity = remaining;
    if (remaining == 0u) capacity = slot->client->limits.stream_chunk_bytes;
    if (slot->file_transfer != NULL) {
      const chttp_file_source_result source_result =
          chttp_file_transfer_read(slot->file_transfer, slot->source_buffer, capacity, &produced);
      if (source_result == CHTTP_FILE_SOURCE_WAIT) return;
      if (source_result == CHTTP_FILE_SOURCE_ERROR) {
        int native_status = 0;
        status = chttp_file_transfer_status(slot->file_transfer, &native_status);
        chttp_slot_fail_and_close(slot, status != SALTS_OK ? status : SALTS_EIO, native_status,
                                  "file-read");
        return;
      }
      if (source_result == CHTTP_FILE_SOURCE_EOF) produced = 0u;
    } else {
      status =
          slot->body_source.read(slot->body_source.user, slot->source_buffer, capacity, &produced);
      if (status != SALTS_OK) {
        chttp_slot_fail_and_close(slot, status, 0, "request-source");
        return;
      }
    }
    if (produced > capacity) {
      chttp_slot_fail_and_close(slot, SALTS_EPROTO, 0, "request-source");
      return;
    }
    if (remaining == 0u) {
      if (produced != 0u) chttp_slot_fail_and_close(slot, SALTS_EPROTO, 0, "request-source-length");
      else chttp_slot_source_finished(slot);
      return;
    }
    if (produced == 0u) {
      chttp_slot_fail_and_close(slot, SALTS_EPROTO, 0, "request-source-length");
      return;
    }
    status = cnet_send(&slot->client->network, slot->connection, slot->source_buffer, produced);
    if (status != SALTS_OK) {
      chttp_slot_fail_and_close(slot, status, 0, "request-source-send");
      return;
    }
    slot->source_transferred += produced;
    return;
  }

  payload = slot->source_buffer + CHTTP_H1_CHUNK_PREFIX_BYTES;
  status = slot->body_source.read(slot->body_source.user, payload, capacity, &produced);
  if (status != SALTS_OK) {
    chttp_slot_fail_and_close(slot, status, 0, "request-source");
    return;
  }
  if (produced > capacity) {
    chttp_slot_fail_and_close(slot, SALTS_EPROTO, 0, "request-source");
    return;
  }
  if (produced == 0u) {
    static const unsigned char final_chunk[] = "0\r\n\r\n";
    memcpy(slot->source_buffer, final_chunk, sizeof(final_chunk) - 1u);
    status = cnet_send(&slot->client->network, slot->connection, slot->source_buffer,
                       sizeof(final_chunk) - 1u);
    if (status != SALTS_OK) {
      chttp_slot_fail_and_close(slot, status, 0, "request-source-send");
      return;
    }
    slot->source_final_pending = true;
    return;
  }
  if (slot->source_transferred > slot->client->limits.max_request_body_bytes ||
      produced > slot->client->limits.max_request_body_bytes - slot->source_transferred) {
    chttp_slot_fail_and_close(slot, SALTS_EMSGSIZE, 0, "request-source-size");
    return;
  }
  {
    const int prefix_chars =
        snprintf((char *)slot->source_buffer, CHTTP_H1_CHUNK_PREFIX_BYTES, "%zx\r\n", produced);
    unsigned char *wire;
    size_t wire_size;
    if (prefix_chars <= 0 || (size_t)prefix_chars >= CHTTP_H1_CHUNK_PREFIX_BYTES) {
      chttp_slot_fail_and_close(slot, SALTS_ERANGE, 0, "request-source-frame");
      return;
    }
    wire = slot->source_buffer + CHTTP_H1_CHUNK_PREFIX_BYTES - (size_t)prefix_chars;
    memmove(wire, slot->source_buffer, (size_t)prefix_chars);
    memcpy(payload + produced, "\r\n", CHTTP_H1_CHUNK_TRAILER_BYTES);
    wire_size = (size_t)prefix_chars + produced + CHTTP_H1_CHUNK_TRAILER_BYTES;
    status = cnet_send(&slot->client->network, slot->connection, wire, wire_size);
  }
  if (status != SALTS_OK) {
    chttp_slot_fail_and_close(slot, status, 0, "request-source-send");
    return;
  }
  slot->source_transferred += produced;
}

static void chttp_slot_file_ready(void *user) {
  chttp_slot *slot = (chttp_slot *)user;
  chttp_slot_source_advance(slot);
}

static void chttp_cnet_send(void *user, cnet_connection connection, size_t size) {
  chttp_slot *slot = (chttp_slot *)user;
  (void)size;
  if (slot == NULL || slot->connection.slot != connection.slot ||
      slot->connection.generation != connection.generation)
    return;
  chttp_slot_source_advance(slot);
}

static void chttp_cnet_state(void *user, cnet_connection connection, cnet_connection_state state,
                             const cnet_error *error) {
  chttp_slot *slot = (chttp_slot *)user;
  int status;
  if (slot == NULL || slot->state == CHTTP_SLOT_FREE || slot->state == CHTTP_SLOT_TERMINAL ||
      slot->connection.slot != connection.slot ||
      slot->connection.generation != connection.generation)
    return;

  if (state == CNET_CONNECTION_CONNECTED) {
    if (slot->state != CHTTP_SLOT_CONNECTING || slot->result_delivered || slot->cancel_requested ||
        slot->client->stop_active)
      return;
    slot->state = CHTTP_SLOT_BUSY;
    if (slot->request_data == NULL || slot->request_size == 0u) {
      chttp_slot_fail_and_close(slot, SALTS_EPROTO, 0, "request-state");
      return;
    }
    status = cnet_send(&slot->client->network, connection, slot->request_data, slot->request_size);
    if (status != SALTS_OK) {
      chttp_slot_fail_and_close(slot, status, 0, "send-admission");
      return;
    }
    free(slot->request_data);
    slot->request_data = NULL;
    slot->request_size = 0u;
    status = chttp_slot_arm_receive(slot);
    if (status != SALTS_OK) chttp_slot_fail_and_close(slot, status, 0, "receive-admission");
    return;
  }

  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;
  slot->receive_armed = false;
  if (!slot->result_delivered) {
    if (slot->cancel_requested) chttp_slot_deliver(slot, NULL, SALTS_ECANCELED, 0, "cancel");
    else if (slot->client->stop_active)
      chttp_slot_deliver(slot, NULL, SALTS_ESHUTDOWN, 0, "shutdown");
    else if (state == CNET_CONNECTION_FAILED) {
      const int failure_status = error != NULL ? error->status : SALTS_EIO;
      const int native_status = error != NULL ? error->native_status : 0;
      const char *stage = error != NULL && error->stage != NULL ? error->stage : "transport";
      chttp_slot_deliver(slot, NULL, failure_status, native_status, stage);
    } else {
      status = chttp_response_parser_finish(&slot->response_parser);
      if (status == SALTS_OK && slot->response_parser.complete &&
          slot->file_sink_transfer != NULL) {
        const chttp_file_sink_result sink_result =
            chttp_file_sink_transfer_advance(slot->file_sink_transfer);
        if (sink_result == CHTTP_FILE_SINK_WAIT) {
          slot->transport_closed = true;
          return;
        }
        if (sink_result == CHTTP_FILE_SINK_ERROR) {
          int native_status = 0;
          status = chttp_file_sink_transfer_status(slot->file_sink_transfer, &native_status);
          chttp_slot_deliver(slot, NULL, status == SALTS_OK ? SALTS_EIO : status, native_status,
                             "file-write");
        } else {
          slot->transport_closed = true;
          chttp_slot_complete_response(slot);
          return;
        }
      } else if (status == SALTS_OK && slot->response_parser.complete) {
        chttp_slot_deliver(slot, &slot->response_parser.response, SALTS_OK, 0, NULL);
      } else {
        chttp_slot_deliver(slot, NULL, status, slot->response_parser.parser_status,
                           slot->response_parser.failure_stage != NULL
                               ? slot->response_parser.failure_stage
                               : "eof");
      }
    }
  }
  slot->state = CHTTP_SLOT_TERMINAL;
  slot->close_pending = false;
}

static void chttp_cnet_receive(void *user, cnet_connection connection,
                               const cnet_receive_view *view) {
  chttp_slot *slot = (chttp_slot *)user;
  int status;
  if (slot == NULL || view == NULL || slot->state == CHTTP_SLOT_FREE ||
      slot->state == CHTTP_SLOT_TERMINAL || slot->connection.slot != connection.slot ||
      slot->connection.generation != connection.generation)
    return;
  slot->receive_armed = false;
  if (slot->state == CHTTP_SLOT_IDLE) {
    (void)chttp_slot_try_close(slot);
    return;
  }
  if (slot->result_delivered) {
    (void)chttp_slot_try_close(slot);
    return;
  }
  if (slot->cancel_requested || slot->client->stop_active) {
    (void)chttp_slot_try_close(slot);
    return;
  }
  if (view->kind != CNET_MESSAGE_BYTES) {
    chttp_slot_fail_and_close(slot, SALTS_ENOTSUP, 0, "transport-kind");
    return;
  }
  status = chttp_response_parser_execute(&slot->response_parser, view->data, view->size);
  if (status != SALTS_OK) {
    chttp_slot_fail_and_close(slot, status, slot->response_parser.parser_status,
                              slot->response_parser.failure_stage != NULL
                                  ? slot->response_parser.failure_stage
                                  : "parse");
    return;
  }
  if (slot->file_sink_transfer != NULL) {
    const chttp_file_sink_result sink_result =
        chttp_file_sink_transfer_advance(slot->file_sink_transfer);
    if (sink_result == CHTTP_FILE_SINK_WAIT) return;
    if (sink_result == CHTTP_FILE_SINK_ERROR) {
      int native_status = 0;
      status = chttp_file_sink_transfer_status(slot->file_sink_transfer, &native_status);
      chttp_slot_fail_and_close(slot, status == SALTS_OK ? SALTS_EIO : status, native_status,
                                "file-write");
      return;
    }
  }
  if (slot->response_parser.complete) {
    chttp_slot_complete_response(slot);
    return;
  }
  status = chttp_slot_arm_receive(slot);
  if (status != SALTS_OK) chttp_slot_fail_and_close(slot, status, 0, "receive-admission");
}

static int chttp_retry_pending_closes(chttp_client_impl *impl) {
  size_t index;
  int first_status = SALTS_OK;
  for (index = 0u; index < impl->request_capacity; ++index) {
    chttp_slot *slot = &impl->slots[index];
    int status;
    if (slot->state == CHTTP_SLOT_FREE || slot->state == CHTTP_SLOT_TERMINAL ||
        !slot->close_pending)
      continue;
    status = chttp_slot_try_close(slot);
    if (status != SALTS_OK && status != SALTS_ENOBUFS && first_status == SALTS_OK)
      first_status = status;
  }
  return first_status;
}

static void chttp_reap_terminal_slots(chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->request_capacity; ++index)
    if (impl->slots[index].state == CHTTP_SLOT_TERMINAL) chttp_slot_release(&impl->slots[index]);
}

static chttp_h2_session *chttp_h2_session_find_free(chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->h2_session_capacity; ++index)
    if (impl->h2_sessions[index].state == CHTTP_H2_SESSION_FREE) return &impl->h2_sessions[index];
  return NULL;
}

static int chttp_begin_idle_eviction(chttp_client_impl *impl) {
  chttp_slot *idle_slot;
  size_t index;
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  idle_slot = chttp_slot_find_any_idle(impl);
  if (idle_slot != NULL) {
    status = chttp_slot_try_close(idle_slot);
    return status == SALTS_ENOBUFS ? SALTS_OK : status;
  }
  for (index = 0u; index < impl->h2_session_capacity; ++index) {
    chttp_h2_session *session = &impl->h2_sessions[index];
    if (session->state != CHTTP_H2_SESSION_ACTIVE || session->active_requests != 0u) continue;
    return chttp_h2_session_begin_stop(session);
  }
  return SALTS_OK;
}

static int chttp_h2_progress_sessions(chttp_client_impl *impl) {
  size_t index;
  int first_status = SALTS_OK;
  for (index = 0u; index < impl->h2_session_capacity; ++index) {
    const int status = chttp_h2_session_progress(&impl->h2_sessions[index]);
    if (status != SALTS_OK && first_status == SALTS_OK) first_status = status;
  }
  return first_status;
}

static bool chttp_h2_sessions_stop_ready(const chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->h2_session_capacity; ++index)
    if (!chttp_h2_session_stop_ready(&impl->h2_sessions[index])) return false;
  return true;
}

static uint32_t chttp_stop_remaining_ms(uint64_t started_ms, uint32_t timeout_ms) {
  const uint64_t elapsed_ms = salts_monotonic_ms() - started_ms;
  return elapsed_ms >= timeout_ms ? 0u : timeout_ms - (uint32_t)elapsed_ms;
}

static void chttp_h2_reap_terminal_sessions(chttp_client_impl *impl) {
  size_t index;
  for (index = 0u; index < impl->h2_session_capacity; ++index) {
    chttp_h2_session *session = &impl->h2_sessions[index];
    if (chttp_h2_session_terminal(session)) chttp_h2_session_destroy(session);
  }
}

static void chttp_h2_complete(void *user, void *request_user, const chttp_response_view *response,
                              int status, int native_status, const char *stage) {
  chttp_client_impl *impl = (chttp_client_impl *)user;
  chttp_slot *slot = (chttp_slot *)request_user;
  if (impl == NULL || slot == NULL || slot->client != impl) return;
  chttp_slot_deliver(slot, response, status, native_status, stage);
  slot->state = CHTTP_SLOT_TERMINAL;
}

static void chttp_h2_file_ready(void *user) {
  chttp_h2_request_state *request = (chttp_h2_request_state *)user;
  (void)chttp_h2_session_resume_file_source(request);
}

static void chttp_h2_file_sink_ready(void *user) {
  chttp_h2_request_state *request = (chttp_h2_request_state *)user;
  (void)chttp_h2_session_resume_file_sink(request);
}

static int chttp_h2_submit(chttp_client_impl *impl, const chttp_request_options *options,
                           chttp_file_transfer *file_transfer,
                           chttp_file_sink_transfer *file_sink_transfer,
                           chttp_request *out_request) {
  chttp_h2_session_callbacks callbacks = {.user = impl, .on_complete = chttp_h2_complete};
  chttp_h2_session *session;
  chttp_tls_profile_impl *tls_profile = NULL;
  chttp_slot *slot;
  size_t session_index;
  size_t slot_index;
  int status;
  if (impl->h2_config_status != SALTS_OK) return impl->h2_config_status;
  status = chttp_h2_stream_uri_supported(options->connection_uri, options->tls != NULL);
  if (status != SALTS_OK) return status;
  status = chttp_tls_profile_acquire(options->tls, &tls_profile);
  if (status != SALTS_OK) return status;
  if (tls_profile != NULL && chttp_tls_profile_protocol(tls_profile) != CHTTP_HTTP_2) {
    chttp_tls_profile_release(tls_profile);
    return SALTS_EPROTONOSUPPORT;
  }
  slot = chttp_slot_find_free(impl);
  if (slot == NULL) {
    chttp_tls_profile_release(tls_profile);
    status = chttp_begin_idle_eviction(impl);
    if (status != SALTS_OK) return status;
    return SALTS_ENOBUFS;
  }
  slot_index = (size_t)(slot - impl->slots);
  slot->generation = chttp_next_generation(slot->generation);
  slot->public_handle =
      (chttp_request){.slot = (uint32_t)(slot_index + 1u), .generation = slot->generation};
  slot->protocol = CHTTP_HTTP_2;
  slot->on_complete = options->on_complete;
  slot->user = options->user;
  slot->state = CHTTP_SLOT_BUSY;
  status = chttp_h2_request_prepare(&slot->h2_request, options, &impl->limits, slot);
  if (status != SALTS_OK) {
    chttp_tls_profile_release(tls_profile);
    chttp_slot_release(slot);
    return status;
  }
  slot->file_transfer = file_transfer;
  slot->file_sink_transfer = file_sink_transfer;
  slot->h2_request.file_transfer = file_transfer;
  slot->h2_request.file_sink_transfer = file_sink_transfer;
  for (session_index = 0u; session_index < impl->h2_session_capacity; ++session_index) {
    session = &impl->h2_sessions[session_index];
    if (!chttp_h2_session_matches(session, options, tls_profile)) continue;
    status = chttp_h2_session_submit(session, &slot->h2_request, options);
    if (status == SALTS_OK) {
      if (file_transfer != NULL)
        chttp_file_transfer_set_ready(file_transfer, chttp_h2_file_ready, &slot->h2_request);
      if (file_sink_transfer != NULL)
        chttp_file_sink_transfer_set_ready(file_sink_transfer, chttp_h2_file_sink_ready,
                                           &slot->h2_request);
      chttp_tls_profile_release(tls_profile);
      *out_request = slot->public_handle;
      return SALTS_OK;
    }
    if (status != SALTS_EBUSY) {
      chttp_tls_profile_release(tls_profile);
      chttp_slot_release(slot);
      return status;
    }
  }
  session = chttp_h2_session_find_free(impl);
  if (session == NULL) {
    chttp_tls_profile_release(tls_profile);
    chttp_slot_release(slot);
    status = chttp_begin_idle_eviction(impl);
    if (status != SALTS_OK) return status;
    return SALTS_ENOBUFS;
  }
  status = chttp_h2_session_open(session, &impl->network, options, tls_profile, &impl->h2_config,
                                 &impl->limits, &callbacks);
  if (status != SALTS_OK) {
    chttp_slot_release(slot);
    if (status == SALTS_ENOBUFS) {
      status = chttp_begin_idle_eviction(impl);
      if (status == SALTS_OK) return SALTS_ENOBUFS;
    }
    return status;
  }
  status = chttp_h2_session_submit(session, &slot->h2_request, options);
  if (status != SALTS_OK) {
    (void)chttp_h2_session_begin_stop(session);
    chttp_slot_release(slot);
    return status;
  }
  if (file_transfer != NULL)
    chttp_file_transfer_set_ready(file_transfer, chttp_h2_file_ready, &slot->h2_request);
  if (file_sink_transfer != NULL)
    chttp_file_sink_transfer_set_ready(file_sink_transfer, chttp_h2_file_sink_ready,
                                       &slot->h2_request);
  *out_request = slot->public_handle;
  return SALTS_OK;
}

int chttp_async_client_init(chttp_async_client *client, const chttp_client_config *config) {
  chttp_client_impl *impl;
  size_t index;
  int status;
  if (client == NULL || config == NULL) return SALTS_EINVAL;
  if (client->impl != NULL) return SALTS_EALREADY;
  if (!chttp_config_valid(config)) return SALTS_EINVAL;
  impl = (chttp_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->slots = (chttp_slot *)calloc(config->request_capacity, sizeof(*impl->slots));
  impl->h2_sessions =
      (chttp_h2_session *)calloc(config->network.connection_capacity, sizeof(*impl->h2_sessions));
  if (impl->slots == NULL || impl->h2_sessions == NULL) {
    free(impl->h2_sessions);
    free(impl->slots);
    free(impl);
    return SALTS_ENOMEM;
  }
  impl->request_capacity = config->request_capacity;
  impl->h2_session_capacity = config->network.connection_capacity;
  impl->h2_config_status = chttp_h2_protocol_config(config, &impl->h2_config);
  impl->file_sink_capacity = config->network.receive_buffer_bytes;
  if (impl->h2_config_status == SALTS_OK &&
      impl->h2_config.input_buffer_bytes > impl->file_sink_capacity)
    impl->file_sink_capacity = impl->h2_config.input_buffer_bytes;
  impl->limits = (chttp_limits){
      .max_start_line_bytes = config->max_start_line_bytes,
      .max_header_count = config->max_header_count,
      .max_header_bytes = config->max_header_bytes,
      .max_request_body_bytes = config->max_request_body_bytes,
      .max_response_body_bytes = config->max_response_body_bytes,
      .max_informational_responses = config->max_informational_responses,
      .max_request_bytes = config->network.max_send_bytes,
      .stream_chunk_bytes =
          config->stream_chunk_bytes != 0u
              ? config->stream_chunk_bytes
              : (config->network.max_send_bytes > CHTTP_H1_CHUNK_OVERHEAD_BYTES
                     ? (config->network.max_send_bytes - CHTTP_H1_CHUNK_OVERHEAD_BYTES <
                                CHTTP_STREAM_CHUNK_DEFAULT_BYTES
                            ? config->network.max_send_bytes - CHTTP_H1_CHUNK_OVERHEAD_BYTES
                            : CHTTP_STREAM_CHUNK_DEFAULT_BYTES)
                     : 0u)};
  for (index = 0u; index < impl->request_capacity; ++index)
    impl->slots[index].client = impl;
  status = cnet_client_init(&impl->network, &config->network);
  if (status != SALTS_OK) {
    free(impl->h2_sessions);
    free(impl->slots);
    free(impl);
    return status;
  }
  impl->admission_open = true;
  client->impl = impl;
  return SALTS_OK;
}

static int chttp_async_client_submit_impl(chttp_async_client *client,
                                          const chttp_request_options *options,
                                          chttp_file_transfer *file_transfer,
                                          chttp_file_sink_transfer *file_sink_transfer,
                                          chttp_request *out_request) {
  chttp_client_impl *impl = chttp_client_get(client);
  chttp_slot *slot;
  unsigned char *request_data = NULL;
  size_t request_size = 0u;
  cnet_connect_options connect_options;
  chttp_tls_profile_impl *tls_profile = NULL;
  size_t slot_index;
  int status;
  if (out_request == NULL) return SALTS_EINVAL;
  *out_request = (chttp_request){0};
  if (impl == NULL || options == NULL || (file_transfer != NULL && options->body_source == NULL) ||
      (file_sink_transfer != NULL && options->body_sink == NULL))
    return SALTS_EINVAL;
  if (impl->callback_active) return SALTS_EBUSY;
  if (!impl->admission_open) return SALTS_ESHUTDOWN;
  if (options->protocol == CHTTP_HTTP_2)
    return chttp_h2_submit(impl, options, file_transfer, file_sink_transfer, out_request);
  if (options->protocol != CHTTP_HTTP_1_1) return SALTS_EINVAL;
  status = chttp_stream_uri_supported(options->connection_uri, options->tls != NULL);
  if (status != SALTS_OK) return status;
  status = chttp_request_build(options, &impl->limits, &request_data, &request_size);
  if (status != SALTS_OK) return status;
  status = chttp_tls_profile_acquire(options->tls, &tls_profile);
  if (status != SALTS_OK) {
    free(request_data);
    return status;
  }
  if (tls_profile != NULL && chttp_tls_profile_protocol(tls_profile) != options->protocol) {
    free(request_data);
    chttp_tls_profile_release(tls_profile);
    return SALTS_EPROTONOSUPPORT;
  }

  slot = chttp_slot_find_idle(impl, options, tls_profile);
  if (slot != NULL) {
    chttp_tls_profile_release(tls_profile);
    if (!slot->receive_armed) {
      free(request_data);
      (void)chttp_slot_try_close(slot);
      return SALTS_ENOBUFS;
    }
    status = chttp_slot_prepare_streaming(slot, options, file_sink_transfer);
    if (status != SALTS_OK) {
      free(request_data);
      return status;
    }
    slot_index = (size_t)(slot - impl->slots);
    slot->generation = chttp_next_generation(slot->generation);
    slot->public_handle =
        (chttp_request){.slot = (uint32_t)(slot_index + 1u), .generation = slot->generation};
    slot->request_data = request_data;
    slot->protocol = CHTTP_HTTP_1_1;
    slot->request_size = request_size;
    slot->on_complete = options->on_complete;
    slot->user = options->user;
    slot->result_delivered = false;
    slot->cancel_requested = false;
    slot->close_admitted = false;
    slot->close_pending = false;
    slot->state = CHTTP_SLOT_BUSY;
    slot->file_transfer = file_transfer;
    slot->file_sink_transfer = file_sink_transfer;
    if (file_transfer != NULL)
      chttp_file_transfer_set_ready(file_transfer, chttp_slot_file_ready, slot);
    if (file_sink_transfer != NULL)
      chttp_file_sink_transfer_set_ready(file_sink_transfer, chttp_slot_file_sink_ready, slot);
    status = cnet_send(&impl->network, slot->connection, slot->request_data, slot->request_size);
    if (status != SALTS_OK) {
      free(slot->request_data);
      slot->request_data = NULL;
      slot->request_size = 0u;
      chttp_response_parser_destroy(&slot->response_parser);
      free(slot->source_buffer);
      slot->source_buffer = NULL;
      slot->source_enabled = false;
      slot->source_complete = false;
      if (slot->file_transfer != NULL)
        chttp_file_transfer_set_ready(slot->file_transfer, NULL, NULL);
      slot->file_transfer = NULL;
      if (slot->file_sink_transfer != NULL)
        chttp_file_sink_transfer_set_ready(slot->file_sink_transfer, NULL, NULL);
      slot->file_sink_transfer = NULL;
      slot->on_complete = NULL;
      slot->user = NULL;
      slot->result_delivered = true;
      slot->state = CHTTP_SLOT_IDLE;
      return status;
    }
    free(slot->request_data);
    slot->request_data = NULL;
    slot->request_size = 0u;
    *out_request = slot->public_handle;
    return SALTS_OK;
  }

  slot = chttp_slot_find_free(impl);
  if (slot == NULL) {
    free(request_data);
    chttp_tls_profile_release(tls_profile);
    status = chttp_begin_idle_eviction(impl);
    if (status != SALTS_OK) return status;
    return SALTS_ENOBUFS;
  }
  status = chttp_slot_prepare_streaming(slot, options, file_sink_transfer);
  if (status != SALTS_OK) {
    free(request_data);
    chttp_tls_profile_release(tls_profile);
    return status;
  }
  slot_index = (size_t)(slot - impl->slots);
  slot->generation = chttp_next_generation(slot->generation);
  slot->public_handle =
      (chttp_request){.slot = (uint32_t)(slot_index + 1u), .generation = slot->generation};
  slot->request_data = request_data;
  slot->protocol = CHTTP_HTTP_1_1;
  slot->request_size = request_size;
  slot->on_complete = options->on_complete;
  slot->user = options->user;
  slot->tls_profile = tls_profile;
  slot->state = CHTTP_SLOT_CONNECTING;
  slot->file_transfer = file_transfer;
  slot->file_sink_transfer = file_sink_transfer;
  if (file_transfer != NULL)
    chttp_file_transfer_set_ready(file_transfer, chttp_slot_file_ready, slot);
  if (file_sink_transfer != NULL)
    chttp_file_sink_transfer_set_ready(file_sink_transfer, chttp_slot_file_sink_ready, slot);
  slot->connection_uri = chttp_copy_text(options->connection_uri);
  slot->authority = chttp_copy_text(options->authority);
  if (slot->connection_uri == NULL || slot->authority == NULL) {
    chttp_slot_release(slot);
    return SALTS_ENOMEM;
  }
  connect_options =
      (cnet_connect_options){.uri = options->connection_uri,
                             .observer = {.on_state = chttp_cnet_state,
                                          .on_receive = chttp_cnet_receive,
                                          .user = slot,
                                          .on_send = chttp_cnet_send},
                             .tls_client = chttp_tls_profile_client(slot->tls_profile)};
  status = cnet_connect(&impl->network, &connect_options, &slot->connection);
  if (status != SALTS_OK) {
    chttp_slot_release(slot);
    if (status == SALTS_ENOBUFS) {
      status = chttp_begin_idle_eviction(impl);
      if (status == SALTS_OK) return SALTS_ENOBUFS;
    }
    return status;
  }
  *out_request = slot->public_handle;
  return SALTS_OK;
}

int chttp_async_client_submit(chttp_async_client *client, const chttp_request_options *options,
                              chttp_request *out_request) {
  return chttp_async_client_submit_impl(client, options, NULL, NULL, out_request);
}

int chttp_async_client_submit_file(chttp_async_client *client, const chttp_request_options *options,
                                   chttp_file_transfer *transfer, chttp_request *out_request) {
  if (transfer == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  return chttp_async_client_submit_impl(client, options, transfer, NULL, out_request);
}

int chttp_async_client_submit_file_download(chttp_async_client *client,
                                            const chttp_request_options *options,
                                            chttp_file_sink_transfer *transfer,
                                            chttp_request *out_request) {
  if (transfer == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  return chttp_async_client_submit_impl(client, options, NULL, transfer, out_request);
}

int chttp_async_request_cancel(chttp_async_client *client, chttp_request request) {
  chttp_client_impl *impl = chttp_client_get(client);
  chttp_slot *slot = chttp_slot_find(impl, request);
  int status;
  if (impl == NULL) return SALTS_EINVAL;
  if (slot == NULL) return SALTS_ENOENT;
  if (slot->result_delivered) return slot->state == CHTTP_SLOT_IDLE ? SALTS_ENOENT : SALTS_EALREADY;
  if (slot->cancel_requested) return SALTS_EALREADY;
  if (slot->protocol == CHTTP_HTTP_2) {
    status = chttp_h2_session_cancel(slot->h2_request.session, &slot->h2_request);
    if (status == SALTS_OK) slot->cancel_requested = true;
    return status;
  }
  status = cnet_close(&impl->network, slot->connection);
  if (status != SALTS_OK) return status;
  slot->cancel_requested = true;
  slot->close_admitted = true;
  slot->state = CHTTP_SLOT_CLOSING;
  return SALTS_OK;
}

int chttp_async_client_poll(chttp_async_client *client, uint32_t timeout_ms,
                            size_t *out_completions) {
  chttp_client_impl *impl = chttp_client_get(client);
  size_t network_events = 0u;
  int status;
  int close_status;
  if (impl == NULL || out_completions == NULL) return SALTS_EINVAL;
  *out_completions = 0u;
  if (impl->poll_active || impl->callback_active) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_ESHUTDOWN;
  impl->poll_active = true;
  impl->completion_count = 0u;
  close_status = chttp_file_runtime_progress(impl);
  if (close_status == SALTS_OK) close_status = chttp_h2_progress_sessions(impl);
  {
    const int retry_status = chttp_retry_pending_closes(impl);
    if (close_status == SALTS_OK) close_status = retry_status;
  }
  status = cnet_client_poll(&impl->network, chttp_file_runtime_poll_timeout(impl, timeout_ms),
                            &network_events);
  if (status == SALTS_OK) {
    int after_status = chttp_file_runtime_progress(impl);
    if (after_status == SALTS_OK) after_status = chttp_h2_progress_sessions(impl);
    if (after_status == SALTS_OK) after_status = chttp_retry_pending_closes(impl);
    if (close_status == SALTS_OK) close_status = after_status;
  }
  chttp_h2_reap_terminal_sessions(impl);
  chttp_reap_terminal_slots(impl);
  *out_completions = impl->completion_count;
  impl->poll_active = false;
  if (status != SALTS_OK) return status;
  return close_status;
}

int chttp_async_client_stop(chttp_async_client *client, uint32_t timeout_ms) {
  chttp_client_impl *impl = chttp_client_get(client);
  const uint64_t started_ms = salts_monotonic_ms();
  int first_status = SALTS_OK;
  int stop_status;
  size_t index;
  if (impl == NULL) return SALTS_EINVAL;
  if (impl->callback_active || impl->poll_active) return SALTS_EBUSY;
  if (impl->stopped) return SALTS_OK;
  impl->admission_open = false;
  impl->stop_active = true;
  impl->completion_count = 0u;
  if (!impl->network_stop_started) {
    for (index = 0u; index < impl->h2_session_capacity; ++index) {
      const int status = chttp_h2_session_begin_stop(&impl->h2_sessions[index]);
      if (status != SALTS_OK && first_status == SALTS_OK) first_status = status;
    }
    {
      int status = chttp_file_runtime_progress(impl);
      if (status == SALTS_OK) status = chttp_h2_progress_sessions(impl);
      if (status != SALTS_OK && first_status == SALTS_OK) first_status = status;
    }
    while (first_status == SALTS_OK && timeout_ms != 0u && !chttp_h2_sessions_stop_ready(impl)) {
      size_t events = 0u;
      const uint32_t remaining_ms = chttp_stop_remaining_ms(started_ms, timeout_ms);
      int status;
      if (remaining_ms == 0u) break;
      status = cnet_client_poll(&impl->network, chttp_file_runtime_poll_timeout(impl, remaining_ms),
                                &events);
      if (status != SALTS_OK) {
        first_status = status;
        break;
      }
      status = chttp_file_runtime_progress(impl);
      if (status == SALTS_OK) status = chttp_h2_progress_sessions(impl);
      if (status != SALTS_OK) first_status = status;
      chttp_h2_reap_terminal_sessions(impl);
      chttp_reap_terminal_slots(impl);
    }
    if (first_status == SALTS_OK && !chttp_h2_sessions_stop_ready(impl)) return SALTS_ETIMEDOUT;
  }
  impl->network_stop_started = true;
  stop_status = cnet_client_stop(&impl->network, chttp_stop_remaining_ms(started_ms, timeout_ms));
  if (stop_status == SALTS_EALREADY) stop_status = SALTS_OK;
  chttp_h2_reap_terminal_sessions(impl);
  chttp_reap_terminal_slots(impl);
  if (stop_status == SALTS_OK && impl->file_runtime_initialized) {
    int file_status = cflow_io_file_runtime_close(&impl->file_runtime);
    if (file_status == SALTS_EALREADY) file_status = SALTS_OK;
    while (file_status == SALTS_OK && !cflow_io_file_runtime_is_quiescent(&impl->file_runtime)) {
      if (timeout_ms != 0u && chttp_stop_remaining_ms(started_ms, timeout_ms) == 0u) {
        file_status = SALTS_ETIMEDOUT;
        break;
      }
      file_status = chttp_file_runtime_progress(impl);
      if (file_status == SALTS_OK) salts_thread_yield();
    }
    if (file_status != SALTS_OK && first_status == SALTS_OK) first_status = file_status;
  }
  if (stop_status == SALTS_OK && first_status == SALTS_OK) impl->stopped = true;
  return first_status != SALTS_OK ? first_status : stop_status;
}

int chttp_async_client_destroy(chttp_async_client *client) {
  chttp_client_impl *impl;
  size_t index;
  int status;
  if (client == NULL) return SALTS_EINVAL;
  impl = chttp_client_get(client);
  if (impl == NULL) return SALTS_OK;
  if (impl->callback_active || impl->poll_active || !impl->stopped) return SALTS_EBUSY;
  if (impl->file_runtime_initialized) {
    status = cflow_io_file_runtime_destroy(&impl->file_runtime);
    if (status != SALTS_OK) return status;
    impl->file_runtime_initialized = false;
  }
  status = cnet_client_destroy(&impl->network);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < impl->h2_session_capacity; ++index)
    chttp_h2_session_destroy(&impl->h2_sessions[index]);
  for (index = 0u; index < impl->request_capacity; ++index)
    chttp_slot_release(&impl->slots[index]);
  free(impl->h2_sessions);
  free(impl->slots);
  free(impl);
  client->impl = NULL;
  return SALTS_OK;
}
