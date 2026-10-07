#include "chttp_h2_server.h"
#include "chttp_jwt_internal.h"
#include "chttp_server_runtime.h"
#include "chttp_cnet_retained.h"
#include "chttp_tls.h"

#include <salts/clock.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

enum {
  CHTTP_SERVER_ERROR_RESPONSE_BYTES = 256,
  CHTTP_SERVER_COOKIE_NAME_BYTES = 64,
  CHTTP_SERVER_GENERATED_RESPONSE_BYTES = 256,
  CHTTP_SERVER_H2_DRAIN_ACK_GRACE_SLICES = 64,
  CHTTP_SERVER_DEFAULT_STREAM_CHUNK_BYTES = 64 * 1024,
  CHTTP_SERVER_CHUNK_PREFIX_RESERVE = 32,
  CHTTP_SERVER_CHUNK_TRAILER_BYTES = 2
};

static const char CHTTP_SERVER_CONTINUE_RESPONSE[] = "HTTP/1.1 100 Continue\r\n\r\n";
static const unsigned char CHTTP_SERVER_H2_PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
SALTS_THREAD_LOCAL chttp_server_impl *chttp_active_callback_server;

static int chttp_server_on_request(void *user, const chttp_server_request_view *request);
static int chttp_server_on_headers(void *user, const chttp_server_request_view *request,
                                   chttp_server_parser_headers_action *out_action);
static int chttp_server_on_continue(void *user);
static int chttp_server_on_body_open(void *user, const chttp_server_request_view *request,
                                     chttp_body_sink *out_sink);
static void chttp_server_on_body_close(void *user, chttp_body_sink *sink, int status);
static int chttp_server_response_stream_next(chttp_server_connection *connection);
static void chttp_server_h1_file_ready(void *user);
static bool chttp_server_should_stop(chttp_server_impl *server);

size_t chttp_server_owner_lease_count(const chttp_server_owner_lane *owner) {
  cnet_handoff_snapshot snapshot;
  if (owner == NULL || owner->handoff.impl == NULL) return 0u;
  if (cnet_handoff_get_snapshot((cnet_handoff *)&owner->handoff, &snapshot) != SALTS_OK)
    return SIZE_MAX;
  return snapshot.reserved + snapshot.queued + snapshot.taken;
}

size_t chttp_server_owner_admission_count(chttp_server_owner_lane *owner) {
  cnet_handoff_snapshot snapshot;
  if (owner == NULL || owner->handoff.impl == NULL) return 0u;
  return cnet_handoff_get_snapshot(&owner->handoff, &snapshot) == SALTS_OK
             ? snapshot.queued : SIZE_MAX;
}

void chttp_server_deadline_start(chttp_server_request_state *state, uint32_t budget_ms) {
  if (budget_ms == 0) {
    state->deadline_ms = 0;
    return;
  }
  const uint64_t now = cmeta_monotonic_ms();
  state->deadline_ms = now > UINT64_MAX - budget_ms ? UINT64_MAX : now + budget_ms;
}

bool chttp_server_deadline_expired(const chttp_server_request_state *state) {
  return state->deadline_ms != 0 && cmeta_monotonic_ms() >= state->deadline_ms;
}

static void chttp_server_on_message_begin(void *user) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  if (connection->request_state.deadline_ms == 0)
    chttp_server_deadline_start(&connection->request_state, connection->server->deadlines.headers_ms);
}

int chttp_server_set_deadlines(chttp_server *server, const chttp_server_deadlines *deadlines) {
  if (server == NULL || server->impl == NULL || deadlines == NULL) return SALTS_EINVAL;
  chttp_server_impl *impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  impl->deadlines = *deadlines;
  return SALTS_OK;
}

int chttp_server_set_admission(chttp_server *server, chttp_server_admission_fn admission, void *user) {
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  chttp_server_impl *impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  impl->admission = admission;
  impl->admission_user = user;
  return SALTS_OK;
}

int chttp_server_request_rejection_response(chttp_server_request_state *state) {
  if (state->admission_result.status_code == 0)
    return chttp_jwt_bearer_unauthorized_response(&state->response);
  int status = SALTS_OK;
  if (state->admission_result.retry_after_seconds != 0) {
    enum { RETRY_AFTER_BYTES = 11 };
    char seconds[RETRY_AFTER_BYTES];
    (void)snprintf(seconds, sizeof(seconds), "%u", (unsigned int)state->admission_result.retry_after_seconds);
    status = chttp_server_response_set_header(&state->response, "Retry-After", seconds);
  }
  return status == SALTS_OK ? chttp_server_reply(&state->response,
      state->admission_result.status_code, NULL, NULL, 0u) : status;
}

static cflow_io_native_backend_kind chttp_server_file_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#elif defined(__APPLE__) && TARGET_OS_OSX
  return CFLOW_IO_NATIVE_DARWIN_AIO;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static void chttp_server_file_wake(void *user) {
  chttp_server_owner_lane *owner = (chttp_server_owner_lane *)user;
  cnet_client *network = chttp_server_owner_network(owner);
  if (network != NULL) (void)cnet_client_wake(network);
}

int chttp_server_file_runtime_ensure(chttp_server_impl *server,
                                     chttp_server_owner_lane *owner,
                                     cflow_io_file_runtime **out_runtime) {
  cflow_io_file_runtime_config config;
  size_t command_capacity;
  int status;
  if (server == NULL || owner == NULL || owner->server != server ||
      chttp_server_owner_network(owner) == NULL || out_runtime == NULL ||
      owner->file_transfer_capacity == 0u)
    return SALTS_EINVAL;
  *out_runtime = NULL;
  if (!owner->file_runtime_initialized) {
    command_capacity = owner->file_transfer_capacity <= SIZE_MAX / 2u
                           ? owner->file_transfer_capacity * 2u
                           : owner->file_transfer_capacity;
    config =
        (cflow_io_file_runtime_config){.backend_kind = chttp_server_file_backend(),
                                       .file_capacity = owner->file_transfer_capacity,
                                       .request_capacity = owner->file_transfer_capacity,
                                       .command_capacity = command_capacity,
                                       .completion_batch_capacity = owner->file_transfer_capacity,
                                       .wake = chttp_server_file_wake,
                                       .wake_user = owner};
    status = cflow_io_file_runtime_init(&owner->file_runtime, &config);
    if (status != SALTS_OK) return status;
    owner->file_runtime_initialized = true;
  }
  *out_runtime = &owner->file_runtime;
  return SALTS_OK;
}

int chttp_server_file_transfer_register(chttp_server_impl *server,
                                        chttp_server_owner_lane *owner,
                                        chttp_file_transfer *transfer) {
  size_t index;
  if (server == NULL || owner == NULL || owner->server != server ||
      owner->file_transfers == NULL || transfer == NULL ||
      transfer->file.impl == NULL)
    return SALTS_EINVAL;
  for (index = 0u; index < owner->file_transfer_capacity; ++index) {
    if (owner->file_transfers[index] == NULL) {
      owner->file_transfers[index] = transfer;
      return SALTS_OK;
    }
  }
  return SALTS_ENOBUFS;
}

static int chttp_server_file_progress(chttp_server_impl *server,
                                      chttp_server_owner_lane *owner) {
  size_t progressed = 0u;
  size_t max_steps;
  size_t index;
  int status;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  if (!owner->file_runtime_initialized) return SALTS_OK;
  max_steps = owner->file_transfer_capacity <= SIZE_MAX / 4u
                  ? owner->file_transfer_capacity * 4u
                  : SIZE_MAX;
  status = cflow_io_file_runtime_run_ready(&owner->file_runtime, max_steps,
                                           &progressed);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < owner->file_transfer_capacity; ++index) {
    chttp_file_transfer *transfer = owner->file_transfers[index];
    if (transfer == NULL) continue;
    if (transfer->owner_release_requested && !transfer->close_requested) {
      status = chttp_file_transfer_close(transfer);
      if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
    }
    if (!transfer->close_requested) continue;
    if (!cflow_io_file_is_quiescent(&transfer->file)) continue;
    status = chttp_file_transfer_destroy(transfer);
    if (status != SALTS_OK) return status;
    free(transfer);
    owner->file_transfers[index] = NULL;
  }
  return SALTS_OK;
}

static bool chttp_server_files_active(const chttp_server_owner_lane *owner) {
  size_t index;
  if (owner == NULL || owner->file_transfers == NULL) return false;
  for (index = 0u; index < owner->file_transfer_capacity; ++index)
    if (owner->file_transfers[index] != NULL) return true;
  return false;
}

static int chttp_server_files_cleanup(chttp_server_impl *server,
                                      chttp_server_owner_lane *owner) {
  size_t index;
  size_t end;
  int status;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  if (!owner->file_runtime_initialized) return SALTS_OK;
  end = chttp_server_owner_connection_end(owner);
  if (owner->connection_begin > server->config.network.connection_capacity ||
      end < owner->connection_begin ||
      end > server->config.network.connection_capacity)
    return SALTS_EINVAL;
  for (index = owner->connection_begin; index < end; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    chttp_server_response_builder *builder =
        &connection->request_state.response_builder;
    if (builder->file_transfer != NULL)
      chttp_server_response_builder_close_source(builder, SALTS_ECANCELED);
    chttp_h2_server_connection_cancel_file_sources(connection->h2);
  }
  for (index = 0u; index < owner->file_transfer_capacity; ++index) {
    chttp_file_transfer *transfer = owner->file_transfers[index];
    if (transfer == NULL) continue;
    chttp_file_transfer_set_ready(transfer, NULL, NULL);
    transfer->owner_release_requested = true;
    status = chttp_file_transfer_close(transfer);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
  }
  while (chttp_server_files_active(owner)) {
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) return status;
    for (index = 0u; index < owner->file_transfer_capacity; ++index) {
      chttp_file_transfer *transfer = owner->file_transfers[index];
      if (transfer == NULL || transfer->close_requested) continue;
      transfer->owner_release_requested = true;
      status = chttp_file_transfer_close(transfer);
      if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
    }
    cmeta_thread_yield();
  }
  status = cflow_io_file_runtime_close(&owner->file_runtime);
  if (status != SALTS_OK && status != SALTS_EALREADY) return status;
  while (!cflow_io_file_runtime_is_quiescent(&owner->file_runtime)) {
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) return status;
    cmeta_thread_yield();
  }
  status = cflow_io_file_runtime_destroy(&owner->file_runtime);
  if (status == SALTS_OK) owner->file_runtime_initialized = false;
  return status;
}

static char *chttp_server_string_copy(const char *value) {
  size_t size;
  char *copy;
  if (value == NULL) return NULL;
  size = strlen(value) + 1u;
  if (size == 0u) return NULL;
  copy = (char *)malloc(size);
  if (copy != NULL) memcpy(copy, value, size);
  return copy;
}

static void chttp_server_buffer_peak_update(chttp_server_impl *server, size_t value) {
  size_t peak = atomic_load_explicit(&server->peak_buffer_bytes, memory_order_relaxed);
  while (peak < value &&
         !atomic_compare_exchange_weak_explicit(&server->peak_buffer_bytes, &peak, value,
                                                memory_order_relaxed, memory_order_relaxed)) {
  }
}

static bool chttp_server_buffer_reserve(chttp_server_impl *server, size_t size) {
  size_t observed;
  if (size == 0u) return true;
  observed = atomic_load_explicit(&server->buffer_bytes, memory_order_relaxed);
  for (;;) {
    size_t desired;
    if (observed > server->config.buffer_capacity_bytes ||
        size > server->config.buffer_capacity_bytes - observed) {
      atomic_fetch_add_explicit(&server->rejected_buffer_allocations, 1u, memory_order_relaxed);
      return false;
    }
    desired = observed + size;
    if (atomic_compare_exchange_weak_explicit(&server->buffer_bytes, &observed, desired,
                                              memory_order_acq_rel, memory_order_relaxed)) {
      chttp_server_buffer_peak_update(server, desired);
      return true;
    }
  }
}

int chttp_server_buffer_grow(void *context, unsigned char **buffer, size_t *capacity,
                             size_t required, size_t limit, size_t preserve_size) {
  chttp_server_impl *server = (chttp_server_impl *)context;
  unsigned char *grown;
  size_t desired;
  size_t delta;
  if (server == NULL || buffer == NULL || capacity == NULL || preserve_size > *capacity)
    return SALTS_EINVAL;
  if (required > limit) return SALTS_EMSGSIZE;
  if (required <= *capacity) return SALTS_OK;
  desired = *capacity == 0u ? 4096u : *capacity;
  if (desired > limit) desired = limit;
  while (desired < required) {
    if (desired > limit - desired) {
      desired = limit;
      break;
    }
    desired *= 2u;
  }
  if (desired < required) desired = required;
  delta = desired - *capacity;
  if (!chttp_server_buffer_reserve(server, delta)) return SALTS_ENOBUFS;
  grown = (unsigned char *)realloc(*buffer, desired);
  if (grown == NULL) {
    atomic_fetch_sub_explicit(&server->buffer_bytes, delta, memory_order_release);
    return SALTS_ENOMEM;
  }
  *buffer = grown;
  *capacity = desired;
  return SALTS_OK;
}

void chttp_server_buffer_release(void *context, unsigned char *buffer, size_t capacity) {
  chttp_server_impl *server = (chttp_server_impl *)context;
  if (buffer == NULL) return;
  free(buffer);
  if (server != NULL && capacity != 0u)
    atomic_fetch_sub_explicit(&server->buffer_bytes, capacity, memory_order_release);
}

int chttp_server_connection_reserve_outbound(chttp_server_connection *connection, size_t required) {
  int status;
  if (connection == NULL || connection->server == NULL) return SALTS_EINVAL;
  if (!chttp_cnet_retained_idle(connection->outbound_retained)) return SALTS_EBUSY;
  status = chttp_server_buffer_grow(
      connection->server, &connection->outbound, &connection->outbound_capacity, required,
      connection->server->config.network.max_send_bytes, connection->outbound_size);
  if (status != SALTS_OK) return status;
  if (connection->outbound == NULL || connection->outbound_capacity == 0u) return SALTS_OK;
  return chttp_cnet_retained_bind(&connection->outbound_retained, connection->outbound,
                                  connection->outbound_capacity);
}

void chttp_server_connection_release_outbound(chttp_server_connection *connection) {
  if (connection == NULL || connection->outbound == NULL) return;
  if (chttp_cnet_retained_release(&connection->outbound_retained) != SALTS_OK) return;
  chttp_server_buffer_release(connection->server, connection->outbound,
                              connection->outbound_capacity);
  connection->outbound = NULL;
  connection->outbound_capacity = 0u;
}

static bool chttp_server_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (right != 0u && left > SIZE_MAX / right)) return false;
  *out = left * right;
  return true;
}

static size_t chttp_server_saturating_add(size_t left, size_t right) {
  return left > SIZE_MAX - right ? SIZE_MAX : left + right;
}

static size_t chttp_server_saturating_multiply(size_t left, size_t right) {
  return right != 0u && left > SIZE_MAX / right ? SIZE_MAX : left * right;
}

static size_t chttp_server_default_buffer_capacity(const chttp_server_config *config) {
  size_t per_connection = config->max_request_body_bytes;
  size_t h2_stream = 0u;
  per_connection = chttp_server_saturating_add(
      per_connection,
      chttp_server_saturating_multiply(config->max_buffered_response_body_bytes, 2u));
  per_connection = chttp_server_saturating_add(per_connection, config->network.max_send_bytes);
  per_connection =
      chttp_server_saturating_add(per_connection, config->network.receive_buffer_bytes);
  if (config->enable_http2) {
    h2_stream = chttp_server_saturating_add(config->max_request_body_bytes,
                                            config->max_buffered_response_body_bytes);
    h2_stream = chttp_server_saturating_add(h2_stream, config->network.max_send_bytes);
    per_connection = chttp_server_saturating_add(
        per_connection, chttp_server_saturating_multiply(h2_stream, config->h2_stream_capacity));
  }
  return chttp_server_saturating_multiply(per_connection, config->network.connection_capacity);
}

static uint32_t chttp_server_poll_timeout(
    const chttp_server_impl *server, const chttp_server_owner_lane *owner) {
  enum { CHTTP_SERVER_FILE_PROGRESS_POLL_MS = 1u };
  if (server != NULL && chttp_server_files_active(owner) &&
      server->config.poll_slice_ms > CHTTP_SERVER_FILE_PROGRESS_POLL_MS)
    return CHTTP_SERVER_FILE_PROGRESS_POLL_MS;
  if (server == NULL) return 0u;
  uint32_t delay = server->config.poll_slice_ms;
  const uint32_t budgets[] = {server->deadlines.headers_ms, server->deadlines.body_ms,
                              server->deadlines.handler_ms};
  for (size_t index = 0; index < sizeof(budgets) / sizeof(budgets[0]); ++index)
    if (budgets[index] != 0 && budgets[index] < delay) delay = budgets[index];
  return delay;
}

static uint64_t chttp_server_deadline_after(uint64_t now_ms, uint64_t delay_ms) {
  return delay_ms > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + delay_ms;
}

static bool chttp_server_cookie_name_valid(const char *name) {
  const unsigned char *cursor = (const unsigned char *)name;
  size_t size = 0u;
  if (cursor == NULL || *cursor == 0u) return false;
  for (; *cursor != 0u; ++cursor, ++size) {
    const unsigned char ch = *cursor;
    if (size >= CHTTP_SERVER_COOKIE_NAME_BYTES) return false;
    if ((ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
        (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
        (ch >= (unsigned char)'a' && ch <= (unsigned char)'z'))
      continue;
    if (strchr("!#$%&'*+-.^_`|~", (int)ch) == NULL) return false;
  }
  return true;
}

static bool chttp_server_power_of_two(size_t value) {
  return value != 0u && (value & (value - 1u)) == 0u;
}

static int chttp_server_config_validate(const chttp_server_config *config) {
  chttp_h2_proto_config h2_config;
  size_t buffered_body_bytes;
  size_t buffered_body_limit;
  const char *cookie_name;
  int status;
  if (config == NULL || config->host == NULL || config->host[0] == '\0' || config->backlog == 0u ||
      config->backlog > INT_MAX || !native_io_backend_kind_supported(config->network.backend) ||
      config->network.connection_capacity == 0u ||
      !chttp_server_power_of_two(config->network.command_capacity) ||
      config->network.request_capacity == 0u || config->network.completion_batch_capacity == 0u ||
      config->network.completion_batch_capacity > config->network.request_capacity ||
      !chttp_server_power_of_two(config->network.event_capacity) ||
      config->network.event_capacity < 2u ||
      config->network.max_send_bytes < CHTTP_SERVER_ERROR_RESPONSE_BYTES ||
      config->network.receive_buffer_bytes == 0u || config->route_capacity == 0u ||
      config->max_target_bytes == 0u || config->max_header_count == 0u ||
      config->max_header_bytes == 0u || config->max_request_body_bytes == 0u ||
      config->max_response_header_count == 0u || config->max_response_header_bytes == 0u ||
      config->max_response_body_bytes == 0u || config->poll_slice_ms == 0u)
    return SALTS_EINVAL;
  if (config->max_buffered_response_body_bytes > config->max_response_body_bytes)
    return SALTS_EINVAL;
  if (config->stream_chunk_bytes != 0u &&
      (config->network.max_send_bytes <=
           CHTTP_SERVER_CHUNK_PREFIX_RESERVE + CHTTP_SERVER_CHUNK_TRAILER_BYTES ||
       config->stream_chunk_bytes > config->network.max_send_bytes -
                                        CHTTP_SERVER_CHUNK_PREFIX_RESERVE -
                                        CHTTP_SERVER_CHUNK_TRAILER_BYTES))
    return SALTS_EMSGSIZE;
  status = chttp_h2_server_config_validate(config, &h2_config);
  if (status != SALTS_OK) return status;
  if (config->tls != NULL) {
    if (config->tls->size != sizeof(*config->tls)) return SALTS_EINVAL;
    const int alpn_status = chttp_tls_server_alpn_validate(
        config->tls->alpn_protocols, config->tls->alpn_protocol_count, config->enable_http2);
    if (alpn_status != SALTS_OK) return alpn_status;
    if (config->network.tls_io_buffer_bytes == 0u || config->network.tls_handshake_timeout_ms == 0u)
      return SALTS_EINVAL;
  }
  if ((config->max_route_param_count != 0u && config->max_route_param_bytes == 0u) ||
      config->max_route_middleware_count > SIZE_MAX / sizeof(chttp_server_middleware) ||
      config->middleware_capacity > SIZE_MAX / sizeof(chttp_server_middleware) ||
      config->max_route_param_count > SIZE_MAX / sizeof(chttp_server_param) ||
      config->route_capacity > SIZE_MAX / sizeof(chttp_server_route_record) ||
      config->network.connection_capacity > SIZE_MAX / sizeof(chttp_server_connection) ||
      config->network.connection_capacity > UINT32_MAX)
    return SALTS_ERANGE;
  if (config->network.max_send_bytes <= sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u ||
      config->max_response_header_bytes >
          config->network.max_send_bytes - (sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u) ||
      CHTTP_SERVER_GENERATED_RESPONSE_BYTES > config->network.max_send_bytes -
                                                  (sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u) -
                                                  config->max_response_header_bytes)
    return SALTS_EMSGSIZE;
  buffered_body_limit = config->network.max_send_bytes -
                        (sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u) -
                        config->max_response_header_bytes - CHTTP_SERVER_GENERATED_RESPONSE_BYTES;
  if (buffered_body_limit == 0u) return SALTS_EMSGSIZE;
  buffered_body_bytes = config->max_buffered_response_body_bytes;
  if (buffered_body_bytes == 0u)
    buffered_body_bytes = config->max_response_body_bytes < buffered_body_limit
                              ? config->max_response_body_bytes
                              : buffered_body_limit;
  if (buffered_body_bytes > buffered_body_limit) return SALTS_EMSGSIZE;
  if (config->session_capacity == 0u) return SALTS_OK;
  cookie_name = config->session_cookie_name == NULL ? "chttp_sid" : config->session_cookie_name;
  if (config->session_entry_capacity == 0u || config->max_session_key_bytes == 0u ||
      config->max_session_value_bytes == 0u || config->session_idle_timeout_ms == 0u ||
      !chttp_server_cookie_name_valid(cookie_name))
    return SALTS_EINVAL;
  return SALTS_OK;
}

int chttp_server_request_state_init(chttp_server_request_state *state, chttp_server_impl *server) {
  int status;
  if (state == NULL || server == NULL) return SALTS_EINVAL;
  *state = (chttp_server_request_state){0};
  state->server = server;
  state->param_storage_capacity = server->config.max_route_param_bytes;
  if (server->config.max_route_param_count != 0u)
    state->params =
        (chttp_server_param *)calloc(server->config.max_route_param_count, sizeof(*state->params));
  if (server->config.max_route_param_bytes != 0u)
    state->param_storage = (char *)malloc(server->config.max_route_param_bytes);
  if ((server->config.max_route_param_count != 0u && state->params == NULL) ||
      (server->config.max_route_param_bytes != 0u && state->param_storage == NULL)) {
    chttp_server_request_state_destroy(state);
    return SALTS_ENOMEM;
  }
  status = chttp_session_context_init(&state->session_context, server);
  if (status != SALTS_OK) {
    chttp_server_request_state_destroy(state);
    return status;
  }
  state->response_builder.server = server;
  status = chttp_server_response_builder_init(&state->response_builder, &server->config);
  if (status != SALTS_OK) {
    chttp_server_request_state_destroy(state);
    return status;
  }
  state->response.impl = &state->response_builder;
  return SALTS_OK;
}

static void chttp_server_request_admission_clear(chttp_server_request_state *state) {
  if (state == NULL) return;
  chttp_jwt_request_state_reset(state);
  state->admitted_route = NULL;
  state->admitted_allowed_methods = 0u;
  state->admitted_fallback_status = 0u;
  state->admission_complete = false;
  state->admission_rejected = false;
  state->admission_result = (chttp_server_admission_result){0};
  state->param_storage_used = 0u;
  state->param_count = 0u;
}

int chttp_server_request_admit(chttp_server_request_state *state,
                               const chttp_server_request_view *request,
                               chttp_method route_method) {
  chttp_server_route_record *route;
  chttp_jwt_bearer_validator *validator;
  unsigned int allowed_methods = 0u;
  int route_status = SALTS_OK;
  int status;

  if (state == NULL || state->server == NULL || request == NULL || state->admission_complete)
    return SALTS_EINVAL;

  chttp_server_stats_request(state->server);
  route =
      chttp_server_route_find(state, route_method, request->path, &allowed_methods, &route_status);
  state->admitted_route = route;
  state->admitted_allowed_methods = allowed_methods;
  state->admitted_fallback_status = allowed_methods != 0u ? 405u : 404u;
  if (route_status == SALTS_ENOBUFS) state->admitted_fallback_status = 414u;
  else if (route_status != SALTS_OK) return route_status;

  state->admission_complete = true;
  validator = route != NULL && route->jwt_bearer_validator != NULL
                  ? route->jwt_bearer_validator
                  : state->server->jwt_bearer_validator;
  if (validator != NULL) {
    status = chttp_jwt_bearer_request_validate(state, request, validator);
    if (status != SALTS_OK) {
      state->admission_rejected = true;
      return status;
    }
  }
  if (state->server->admission == NULL) return SALTS_OK;
  chttp_server_request_view admitted = *request;
  admitted.params = state->params;
  admitted.param_count = state->param_count;
  admitted.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
  admitted.session = NULL;
  admitted.body = NULL;
  admitted.body_size = 0;
  admitted.body_sink_user = NULL;
  chttp_server_impl *previous = chttp_active_callback_server;
  chttp_active_callback_server = state->server;
  status = state->server->admission(state->server->admission_user, &admitted, &state->admission_result);
  chttp_active_callback_server = previous;
  const unsigned int code = state->admission_result.status_code;
  if (status != SALTS_OK || (code != 0 && (code < 400u || code > 599u)) ||
      (code == 0 && state->admission_result.retry_after_seconds != 0)) {
    state->admission_result = (chttp_server_admission_result){.status_code = 500u};
    chttp_server_stats_handler_error(state->server);
  }
  state->admission_rejected = state->admission_result.status_code != 0;
  return state->admission_rejected ? SALTS_EPERM : SALTS_OK;
}

void chttp_server_request_state_reset(chttp_server_request_state *state) {
  if (state == NULL) return;
  chttp_server_request_body_close(state, SALTS_ECANCELED);
  chttp_server_request_admission_clear(state);
  state->deadline_ms = 0;
  chttp_server_response_builder_reset(&state->response_builder);
  state->param_storage_used = 0u;
  state->param_count = 0u;
  state->session.impl = NULL;
  chttp_session_context_reset(&state->session_context);
  state->body_route = NULL;
  state->body_sink = (chttp_body_sink){0};
  state->body_sink_user = NULL;
  state->body_was_streamed = false;
}

void chttp_server_request_state_destroy(chttp_server_request_state *state) {
  if (state == NULL) return;
  chttp_server_request_body_close(state, SALTS_ECANCELED);
  chttp_jwt_request_state_reset(state);
  chttp_server_response_builder_destroy(&state->response_builder);
  chttp_session_context_destroy(&state->session_context);
  free(state->param_storage);
  free(state->params);
  *state = (chttp_server_request_state){0};
}

static void chttp_server_connection_destroy(chttp_server_connection *connection) {
  if (connection == NULL) return;
  chttp_server_websocket_reset(connection);
  chttp_h2_server_connection_destroy(connection->h2);
  chttp_server_parser_destroy(&connection->parser);
  chttp_server_request_state_destroy(&connection->request_state);
  chttp_server_response_builder_destroy(&connection->deferred_builder);
  chttp_server_buffer_release(connection->server, connection->websocket_upgrade_input,
                              connection->websocket_upgrade_input_capacity);
  chttp_server_connection_release_outbound(connection);
  *connection = (chttp_server_connection){0};
}

static void chttp_server_owner_storage_release(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t index;
  if (server == NULL || owner == NULL) return;
  /* Storage is released before startup or after joining all runtime threads.
   * Active credit obligations are rejected by server_destroy before this call. */
  if (owner->handoff.impl != NULL) (void)cnet_handoff_destroy(&owner->handoff);
  if (owner->websocket_commands != NULL)
    for (index = 0u; index < server->config.network.command_capacity; ++index)
      free(owner->websocket_commands[index].data);
  free(owner->file_transfers);
  free(owner->websocket_commands);
  owner->file_transfers = NULL;
  owner->websocket_commands = NULL;
  owner->file_transfer_capacity = 0u;
  owner->websocket_command_head = 0u;
  owner->websocket_command_count = 0u;
}

static int chttp_server_owner_storage_prepare(
    chttp_server_impl *server, chttp_server_owner_lane *owner,
    size_t owner_index, size_t owner_count, bool primary) {
  size_t begin;
  size_t connection_count;
  size_t file_transfer_capacity;
  const size_t capacity =
      server != NULL ? server->config.network.connection_capacity : 0u;
  if (server == NULL || owner == NULL || owner_count == 0u ||
      owner_index >= owner_count)
    return SALTS_EINVAL;
  {
    const size_t base = capacity / owner_count;
    const size_t remainder = capacity % owner_count;
    connection_count = base + (owner_index < remainder ? 1u : 0u);
    begin = base * owner_index +
            (owner_index < remainder ? owner_index : remainder);
  }
  if (connection_count == 0u || begin > capacity ||
      connection_count > capacity - begin)
    return SALTS_EINVAL;
  file_transfer_capacity = connection_count;
  if (server->config.enable_http2 &&
      !chttp_server_multiply(file_transfer_capacity,
                             server->config.h2_stream_capacity,
                             &file_transfer_capacity))
    return SALTS_ERANGE;
  if (file_transfer_capacity == 0u ||
      file_transfer_capacity > SIZE_MAX / sizeof(chttp_file_transfer *))
    return SALTS_ERANGE;

  *owner = (chttp_server_owner_lane){
      .server = server,
      .network = primary ? &server->network : NULL,
      .connection_begin = begin,
      .connection_count = connection_count,
      .file_transfer_capacity = file_transfer_capacity,
      .terminal_status = SALTS_OK};
  atomic_init(&owner->runtime_state, CHTTP_SERVER_OWNER_RUNTIME_IDLE);
  const cnet_handoff_config handoff_config = {sizeof(handoff_config),
      CNET_HANDOFF_VERSION, connection_count, connection_count};
  const int handoff_status = cnet_handoff_init(&owner->handoff, &handoff_config);
  if (handoff_status != SALTS_OK) return handoff_status;
  owner->file_transfers = (chttp_file_transfer **)calloc(
      file_transfer_capacity, sizeof(*owner->file_transfers));
  owner->websocket_commands = (chttp_server_websocket_command *)calloc(
      server->config.network.command_capacity, sizeof(*owner->websocket_commands));
  if (owner->file_transfers == NULL ||
      owner->websocket_commands == NULL) {
    chttp_server_owner_storage_release(server, owner);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int chttp_server_owner_topology_configure(
    chttp_server_impl *server, size_t owner_count) {
  chttp_server_owner_lane primary = {0};
  chttp_server_owner_lane *additional = NULL;
  size_t owner_index;
  size_t connection_index;
  int status;

  if (server == NULL || owner_count == 0u ||
      owner_count > server->config.network.connection_capacity)
    return SALTS_EINVAL;
  if (owner_count > 1u) {
    if (owner_count - 1u > SIZE_MAX / sizeof(*additional))
      return SALTS_ERANGE;
    additional = (chttp_server_owner_lane *)calloc(
        owner_count - 1u, sizeof(*additional));
    if (additional == NULL) return SALTS_ENOMEM;
  }

  status = chttp_server_owner_storage_prepare(
      server, &primary, 0u, owner_count, true);
  if (status != SALTS_OK) goto fail;
  for (owner_index = 1u; owner_index < owner_count; ++owner_index) {
    status = chttp_server_owner_storage_prepare(
        server, &additional[owner_index - 1u], owner_index, owner_count, false);
    if (status != SALTS_OK) goto fail;
  }

  for (owner_index = 0u; owner_index < server->owner_count; ++owner_index)
    chttp_server_owner_storage_release(
        server, chttp_server_owner_at(server, owner_index));
  free(server->additional_owners);

  server->owner = primary;
  primary = (chttp_server_owner_lane){0};
  server->additional_owners = additional;
  server->owner_count = owner_count;

  if (server->connections != NULL) {
    for (owner_index = 0u; owner_index < owner_count; ++owner_index) {
      chttp_server_owner_lane *owner = chttp_server_owner_at(server, owner_index);
      const size_t end = chttp_server_owner_connection_end(owner);
      for (connection_index = owner->connection_begin;
           connection_index < end; ++connection_index)
        server->connections[connection_index].owner = owner;
    }
  }
  return SALTS_OK;

fail:
  chttp_server_owner_storage_release(server, &primary);
  if (additional != NULL) {
    for (owner_index = 1u; owner_index < owner_count; ++owner_index)
      chttp_server_owner_storage_release(
          server, &additional[owner_index - 1u]);
  }
  free(additional);
  return status;
}

static void chttp_server_impl_free(chttp_server_impl *impl) {
  size_t index;
  if (impl == NULL) return;
  if (impl->connections != NULL)
    for (index = 0u; index < impl->config.network.connection_capacity; ++index)
      chttp_server_connection_destroy(&impl->connections[index]);
  chttp_session_store_destroy(impl);
  for (index = 0u; index < impl->owner_count; ++index)
    chttp_server_owner_storage_release(impl, chttp_server_owner_at(impl, index));
  free(impl->additional_owners);
  free(impl->connections);
  free(impl->middleware);
  free(impl->route_middleware);
  free(impl->route_paths);
  free(impl->routes);
  free(impl->session_cookie_name);
  free(impl->host);
  if (impl->tls_initialized) (void)cnet_tls_server_destroy(&impl->tls_server);
  if (impl->sync_initialized) {
    cmeta_cond_destroy(&impl->changed);
    cmeta_mutex_destroy(&impl->mutex);
  }
  free(impl);
}

static int chttp_server_connection_init(chttp_server_impl *server,
                                        chttp_server_connection *connection) {
  const chttp_server_parser_config parser_config = {
      .max_target_bytes = server->config.max_target_bytes,
      .max_header_count = server->config.max_header_count,
      .max_header_bytes = server->config.max_header_bytes,
      .max_body_bytes = server->config.max_request_body_bytes,
      .on_request = chttp_server_on_request,
      .on_headers = chttp_server_on_headers,
      .on_message_begin = chttp_server_on_message_begin,
      .on_continue = chttp_server_on_continue,
      .on_body_open = chttp_server_on_body_open,
      .on_body_close = chttp_server_on_body_close,
      .on_upgrade = chttp_server_websocket_upgrade,
      .buffer_grow = chttp_server_buffer_grow,
      .buffer_release = chttp_server_buffer_release,
      .buffer_context = server,
      .user = connection};
  int status;
  connection->server = server;
  connection->owner = &server->owner;
  status = chttp_server_request_state_init(&connection->request_state, server);
  if (status != SALTS_OK) return status;
  connection->request_state.response_builder.connection = connection;
  connection->deferred_builder.server = server;
  status = chttp_server_response_builder_init(&connection->deferred_builder, &server->config);
  if (status != SALTS_OK) return status;
  connection->deferred_response.impl = &connection->deferred_builder;
  atomic_init(&connection->deferred_token,
              chttp_server_deferred_token(0u, CHTTP_SERVER_DEFERRED_IDLE));
  connection->deferred_target =
      (chttp_server_deferred_target){.server = server,
                                     .connection = connection,
                                     .request_state = &connection->request_state,
                                     .response_builder = &connection->deferred_builder,
                                     .response = &connection->deferred_response,
                                     .token = &connection->deferred_token,
                                     .kind = CHTTP_SERVER_DEFERRED_HTTP_1_1};
  connection->request_state.response_builder.deferred_target = &connection->deferred_target;
  status = chttp_server_parser_init(&connection->parser, &parser_config);
  if (status == SALTS_OK && server->config.enable_http2)
    status = chttp_h2_server_connection_init(&connection->h2, connection);
  return status;
}

int chttp_server_init(chttp_server *server, const chttp_server_config *config) {
  chttp_server_impl *impl;
  const char *cookie_name;
  size_t route_path_stride;
  size_t route_path_bytes;
  size_t route_middleware_count;
  size_t file_transfer_capacity;
  size_t index;
  int status;
  if (server == NULL) return SALTS_EINVAL;
  if (server->impl != NULL) return SALTS_EALREADY;
  status = chttp_server_config_validate(config);
  if (status != SALTS_OK) return status;
  route_path_stride = config->max_target_bytes + 1u;
  file_transfer_capacity = config->network.connection_capacity;
  if (config->enable_http2 &&
      (!chttp_server_multiply(config->network.connection_capacity, config->h2_stream_capacity,
                              &file_transfer_capacity) ||
       file_transfer_capacity == 0u))
    return SALTS_ERANGE;
  if (route_path_stride == 0u ||
      !chttp_server_multiply(config->route_capacity, route_path_stride, &route_path_bytes) ||
      !chttp_server_multiply(config->route_capacity, config->max_route_middleware_count,
                             &route_middleware_count) ||
      (route_middleware_count != 0u &&
       route_middleware_count > SIZE_MAX / sizeof(chttp_server_middleware)) ||
      file_transfer_capacity > SIZE_MAX / sizeof(chttp_file_transfer *))
    return SALTS_ERANGE;
  impl = (chttp_server_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->config = *config;
  impl->execution_options =
      (chttp_server_execution_options)CHTTP_SERVER_EXECUTION_OPTIONS_INIT;
  impl->owner = (chttp_server_owner_lane){
      .server = impl,
      .network = &impl->network,
      .connection_begin = 0u,
      .connection_count = config->network.connection_capacity,
      .file_transfer_capacity = file_transfer_capacity,
      .terminal_status = SALTS_OK};
  atomic_init(&impl->owner.runtime_state, CHTTP_SERVER_OWNER_RUNTIME_IDLE);
  impl->owner_count = 1u;
  impl->socket_options = (chttp_server_socket_options)CHTTP_SERVER_SOCKET_OPTIONS_INIT;
  if (impl->config.stream_chunk_bytes == 0u) {
    const size_t transport_chunk_bytes = config->network.max_send_bytes -
                                         CHTTP_SERVER_CHUNK_PREFIX_RESERVE -
                                         CHTTP_SERVER_CHUNK_TRAILER_BYTES;
    impl->config.stream_chunk_bytes =
        transport_chunk_bytes < CHTTP_SERVER_DEFAULT_STREAM_CHUNK_BYTES
            ? transport_chunk_bytes
            : CHTTP_SERVER_DEFAULT_STREAM_CHUNK_BYTES;
  }
  if (impl->config.max_buffered_response_body_bytes == 0u) {
    const size_t buffered_body_limit =
        config->network.max_send_bytes - (sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u) -
        config->max_response_header_bytes - CHTTP_SERVER_GENERATED_RESPONSE_BYTES;
    impl->config.max_buffered_response_body_bytes =
        config->max_response_body_bytes < buffered_body_limit ? config->max_response_body_bytes
                                                              : buffered_body_limit;
  }
  if (impl->config.buffer_capacity_bytes == 0u)
    impl->config.buffer_capacity_bytes = chttp_server_default_buffer_capacity(&impl->config);
  atomic_init(&impl->buffer_bytes, 0u);
  atomic_init(&impl->peak_buffer_bytes, 0u);
  atomic_init(&impl->rejected_buffer_allocations, 0u);
  if (config->tls != NULL) {
    status = cnet_tls_server_init(&impl->tls_server, config->tls);
    if (status != SALTS_OK) {
      chttp_server_impl_free(impl);
      return status;
    }
    impl->tls_initialized = true;
    impl->config.tls = NULL;
  }
  impl->max_response_wire_bytes = impl->config.max_buffered_response_body_bytes +
                                  impl->config.max_response_header_bytes +
                                  CHTTP_SERVER_GENERATED_RESPONSE_BYTES;
  cookie_name = config->session_cookie_name == NULL ? "chttp_sid" : config->session_cookie_name;
  impl->host = chttp_server_string_copy(config->host);
  impl->session_cookie_name = chttp_server_string_copy(cookie_name);
  impl->routes = (chttp_server_route_record *)calloc(config->route_capacity, sizeof(*impl->routes));
  impl->route_paths = (char *)calloc(route_path_bytes, 1u);
  if (route_middleware_count != 0u)
    impl->route_middleware =
        (chttp_server_middleware *)calloc(route_middleware_count, sizeof(*impl->route_middleware));
  if (config->middleware_capacity != 0u)
    impl->middleware =
        (chttp_server_middleware *)calloc(config->middleware_capacity, sizeof(*impl->middleware));
  impl->connections = (chttp_server_connection *)calloc(config->network.connection_capacity,
                                                        sizeof(*impl->connections));
  const cnet_handoff_config handoff_config = {sizeof(handoff_config),
      CNET_HANDOFF_VERSION, config->network.connection_capacity,
      config->network.connection_capacity};
  status = cnet_handoff_init(&impl->owner.handoff, &handoff_config);
  if (status != SALTS_OK) {
    chttp_server_impl_free(impl);
    return status;
  }
  impl->owner.file_transfers =
      (chttp_file_transfer **)calloc(file_transfer_capacity,
                                     sizeof(*impl->owner.file_transfers));
  impl->owner.websocket_commands =
      (chttp_server_websocket_command *)calloc(
          config->network.command_capacity,
          sizeof(*impl->owner.websocket_commands));
  if (impl->host == NULL || impl->session_cookie_name == NULL || impl->routes == NULL ||
      impl->route_paths == NULL ||
      (route_middleware_count != 0u && impl->route_middleware == NULL) ||
      (config->middleware_capacity != 0u && impl->middleware == NULL) ||
      impl->connections == NULL ||
      impl->owner.file_transfers == NULL ||
      impl->owner.websocket_commands == NULL) {
    chttp_server_impl_free(impl);
    return SALTS_ENOMEM;
  }
  impl->config.host = impl->host;
  impl->config.session_cookie_name = impl->session_cookie_name;
  for (index = 0u; index < config->route_capacity; ++index) {
    impl->routes[index].path = impl->route_paths + index * route_path_stride;
    if (config->max_route_middleware_count != 0u)
      impl->routes[index].middleware =
          impl->route_middleware + index * config->max_route_middleware_count;
  }
  status = chttp_session_store_init(impl);
  if (status != SALTS_OK) {
    chttp_server_impl_free(impl);
    return status;
  }
  for (index = 0u; index < config->network.connection_capacity; ++index) {
    impl->connections[index].server_slot = (uint32_t)(index + 1u);
    impl->connections[index].server_generation = 0u;
    status = chttp_server_connection_init(impl, &impl->connections[index]);
    if (status != SALTS_OK) {
      chttp_server_impl_free(impl);
      return status;
    }
  }
  cmeta_mutex_init(&impl->mutex);
  cmeta_cond_init(&impl->changed);
  impl->sync_initialized = true;
  impl->stats.terminal_status = SALTS_OK;
  server->impl = impl;
  return SALTS_OK;
}

int chttp_server_set_socket_options(chttp_server *server,
                                    const chttp_server_socket_options *options) {
  chttp_server_impl *impl;
  int status;
  if (server == NULL || server->impl == NULL || options == NULL ||
      options->size != sizeof(*options))
    return SALTS_EINVAL;
  status = cnet_stream_socket_options_validate(&options->stream);
  if (status != SALTS_OK) return status;
  status = cnet_listener_options_validate(&options->listener);
  if (status != SALTS_OK) return status;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called || impl->thread_started || impl->network_initialized ||
      impl->listener_initialized)
    return SALTS_EBUSY;
  impl->socket_options = *options;
  return SALTS_OK;
}

int chttp_server_set_execution_options(
    chttp_server *server, const chttp_server_execution_options *options) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || options == NULL ||
      options->size != sizeof(*options) ||
      options->version != CHTTP_SERVER_EXECUTION_OPTIONS_VERSION)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (options->owner_count == 0u ||
      options->owner_count > impl->config.network.connection_capacity)
    return SALTS_EINVAL;
  if (impl->start_called || impl->thread_started || impl->network_initialized ||
      impl->listener_initialized)
    return SALTS_EBUSY;
  if (options->owner_count != impl->owner_count) {
    const int status =
        chttp_server_owner_topology_configure(impl, options->owner_count);
    if (status != SALTS_OK) return status;
  }
  impl->execution_options = *options;
  return SALTS_OK;
}

static void chttp_server_stats_update(chttp_server_impl *server, int field) {
  cmeta_mutex_lock(&server->mutex);
  switch (field) {
  case 1:
    ++server->stats.accepted_connections;
    ++server->stats.active_connections;
    break;
  case 2:
    if (server->stats.active_connections != 0u) --server->stats.active_connections;
    break;
  case 3:
    ++server->stats.requests;
    break;
  case 4:
    ++server->stats.responses;
    break;
  case 5:
    ++server->stats.protocol_errors;
    break;
  case 6:
    ++server->stats.handler_errors;
    break;
  default:
    break;
  }
  cmeta_mutex_unlock(&server->mutex);
}

void chttp_server_stats_connection_open(chttp_server_impl *server) {
  chttp_server_stats_update(server, 1);
}
void chttp_server_stats_connection_close(chttp_server_impl *server) {
  chttp_server_stats_update(server, 2);
}
void chttp_server_stats_request(chttp_server_impl *server) { chttp_server_stats_update(server, 3); }
void chttp_server_stats_response(chttp_server_impl *server) {
  chttp_server_stats_update(server, 4);
}
void chttp_server_stats_protocol_error(chttp_server_impl *server) {
  chttp_server_stats_update(server, 5);
}
void chttp_server_stats_handler_error(chttp_server_impl *server) {
  chttp_server_stats_update(server, 6);
}

static bool chttp_server_connection_matches(const chttp_server_connection *connection,
                                            cnet_connection handle) {
  return connection != NULL && connection->active && connection->handle.slot == handle.slot &&
         connection->handle.generation == handle.generation;
}

static bool chttp_server_action_pressure(int status) {
  return status == SALTS_ENOBUFS || status == SALTS_EBUSY;
}

static int chttp_server_connection_retry(chttp_server_connection *connection) {
  chttp_server_pending_action action;
  int status;
  if (connection == NULL || !connection->active || !connection->connected || connection->writing)
    return SALTS_OK;
  action = connection->pending_action;
  if (action == CHTTP_SERVER_PENDING_NONE) return SALTS_OK;
  if (action == CHTTP_SERVER_PENDING_RECEIVE)
    status = cnet_receive(chttp_server_connection_network(connection), connection->handle, 1u);
  else if (action == CHTTP_SERVER_PENDING_SEND) {
    if (connection->retained_response_sg) {
      chttp_server_response_builder *builder =
          &connection->request_state.response_builder;
      if (builder->retained_body == NULL ||
          mem_buffer_used(builder->retained_body) !=
              connection->retained_response_sg_body_size)
        return SALTS_EPROTO;
      status = chttp_cnet_retained_send_pair(
          chttp_server_connection_network(connection), connection->handle,
          connection->outbound_retained, connection->outbound,
          connection->outbound_capacity, connection->outbound_size,
          builder->retained_body, connection->close_after_write ? 1 : 0);
    } else {
      status = chttp_cnet_retained_send(
          chttp_server_connection_network(connection), connection->handle,
          connection->outbound_retained, connection->outbound,
          connection->outbound_capacity, connection->outbound_size,
          connection->close_after_write ? 1 : 0);
    }
  } else status = cnet_close(chttp_server_connection_network(connection), connection->handle);
  if (status == SALTS_OK) {
    connection->pending_action = CHTTP_SERVER_PENDING_NONE;
    if (action == CHTTP_SERVER_PENDING_SEND) connection->writing = true;
    return SALTS_OK;
  }
  if (action == CHTTP_SERVER_PENDING_CLOSE &&
      (status == SALTS_EALREADY || status == SALTS_ESHUTDOWN)) {
    connection->pending_action = CHTTP_SERVER_PENDING_NONE;
    return SALTS_OK;
  }
  return status;
}

void chttp_server_connection_close(chttp_server_connection *connection) {
  int status;
  if (connection == NULL || !connection->active) return;
  connection->close_after_write = true;
  if (connection->pending_action != CHTTP_SERVER_PENDING_SEND)
    connection->pending_action = CHTTP_SERVER_PENDING_CLOSE;
  status = chttp_server_connection_retry(connection);
  (void)status;
}

static int chttp_server_connection_receive(chttp_server_connection *connection) {
  int status;
  if (connection == NULL || connection->close_after_write) return SALTS_ESHUTDOWN;
  connection->pending_action = CHTTP_SERVER_PENDING_RECEIVE;
  status = chttp_server_connection_retry(connection);
  if (status != SALTS_OK && !chttp_server_action_pressure(status))
    chttp_server_connection_close(connection);
  return status;
}

static int chttp_server_select_tls_protocol(chttp_server_connection *connection) {
  char alpn[16];
  size_t alpn_size = 0u;
  int status;
  if (!connection->server->tls_initialized) {
    connection->wire_protocol = connection->server->config.enable_http2
                                    ? CHTTP_SERVER_WIRE_UNKNOWN
                                    : CHTTP_SERVER_WIRE_HTTP_1_1;
    return SALTS_OK;
  }
  if (!connection->server->config.enable_http2) {
    connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_1_1;
    return SALTS_OK;
  }
  status = cnet_tls_negotiated_alpn(chttp_server_connection_network(connection), connection->handle, alpn,
                                    sizeof(alpn), &alpn_size);
  if (status == SALTS_ENOENT) {
    connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_1_1;
    return SALTS_OK;
  }
  if (status != SALTS_OK) return status;
  if (alpn_size == sizeof("h2") - 1u && memcmp(alpn, "h2", alpn_size) == 0) {
    connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_2;
    return connection->h2 == NULL ? SALTS_EPROTO : SALTS_OK;
  }
  if (alpn_size == sizeof("http/1.1") - 1u && memcmp(alpn, "http/1.1", alpn_size) == 0) {
    connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_1_1;
    return SALTS_OK;
  }
  return SALTS_EPROTONOSUPPORT;
}

void chttp_server_request_enrich(const chttp_server_connection *connection,
                                 chttp_server_request_view *request) {
  if (connection == NULL || request == NULL) return;
  request->peer = &connection->peer;
  request->peer_certificate_sha256 =
      connection->peer_certificate_sha256[0] != '\0' ? connection->peer_certificate_sha256 : NULL;
}

static void chttp_server_on_state(void *user, cnet_connection handle, cnet_connection_state state,
                                  const cnet_error *error) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  (void)error;
  if (!chttp_server_connection_matches(connection, handle)) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    int status = chttp_server_select_tls_protocol(connection);
    connection->peer_certificate_sha256[0] = '\0';
    if (status == SALTS_OK && connection->server->tls_initialized) {
      status = cnet_tls_peer_certificate_sha256(chttp_server_connection_network(connection), connection->handle,
                                                connection->peer_certificate_sha256);
      if (status == SALTS_ENOENT) status = SALTS_OK;
    }
    connection->connected = true;
    if (status == SALTS_OK) (void)chttp_server_connection_receive(connection);
    else {
      chttp_server_stats_protocol_error(connection->server);
      chttp_server_connection_close(connection);
    }
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    const chttp_server_deferred_state deferred_state = chttp_server_deferred_token_state(
        atomic_load_explicit(&connection->deferred_token, memory_order_acquire));
    chttp_server_websocket_transport_closed(connection);
    connection->active = false;
    connection->connected = false;
    connection->writing = false;
    connection->close_after_write = false;
    connection->response_streaming = false;
    connection->response_source_chunked = false;
    connection->response_close_after_stream = false;
    connection->deferred_response_writing = false;
    connection->retained_response_paused = false;
    connection->retained_response_sg = false;
    connection->retained_response_sg_body_size = 0u;
    connection->pending_action = CHTTP_SERVER_PENDING_NONE;
    connection->outbound_size = 0u;
    chttp_server_connection_release_outbound(connection);
    connection->h2_close_after_ms = 0u;
    connection->protocol_prefix_size = 0u;
    connection->wire_protocol = CHTTP_SERVER_WIRE_UNKNOWN;
    connection->peer = (cnet_stream_peer){0};
    connection->peer_certificate_sha256[0] = '\0';
    connection->handle = (cnet_connection){0};
    chttp_h2_server_connection_release(connection->h2);
    connection->websocket_upgrade_input_size = 0u;
    chttp_server_buffer_release(connection->server, connection->websocket_upgrade_input,
                                connection->websocket_upgrade_input_capacity);
    connection->websocket_upgrade_input = NULL;
    connection->websocket_upgrade_input_capacity = 0u;
    if (deferred_state == CHTTP_SERVER_DEFERRED_IDLE) {
      chttp_server_request_state_reset(&connection->request_state);
      chttp_server_response_builder_reset(&connection->deferred_builder);
      (void)chttp_server_parser_reset(&connection->parser);
    } else {
      connection->deferred_disconnected = true;
    }
    if (connection->owner_ticket.slot != 0u) {
      (void)cnet_handoff_release(&connection->owner->handoff, connection->owner_ticket);
      connection->owner_ticket = (cnet_handoff_ticket){0};
    }
    chttp_server_stats_connection_close(connection->server);
  }
}

int chttp_server_send_pending(chttp_server_connection *connection) {
  int status;
  if (chttp_server_deferred_token_state(atomic_load_explicit(
          &connection->deferred_token, memory_order_acquire)) != CHTTP_SERVER_DEFERRED_IDLE)
    return SALTS_OK;
  if (connection->writing) return SALTS_OK;
  if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_2 && connection->outbound_size == 0u) {
    status = chttp_h2_server_connection_flush(connection->h2);
    if (status != SALTS_OK) {
      chttp_server_connection_close(connection);
      return status;
    }
    if (connection->outbound_size == 0u && chttp_h2_server_connection_stop_ready(connection->h2)) {
      connection->h2_close_after_ms = 0u;
      chttp_server_connection_close(connection);
      return SALTS_OK;
    }
    if (connection->outbound_size == 0u &&
        chttp_h2_server_connection_stop_waiting(connection->h2)) {
      if (connection->h2_close_after_ms == 0u) {
        const uint64_t quiet_ms = (uint64_t)connection->server->config.poll_slice_ms *
                                  CHTTP_SERVER_H2_DRAIN_ACK_GRACE_SLICES;
        connection->h2_close_after_ms = chttp_server_deadline_after(cmeta_monotonic_ms(), quiet_ms);
      }
      return chttp_server_connection_receive(connection);
    }
  }
  if (connection->outbound_size == 0u) return chttp_server_connection_receive(connection);
  connection->pending_action = CHTTP_SERVER_PENDING_SEND;
  status = chttp_server_connection_retry(connection);
  if (status != SALTS_OK && !chttp_server_action_pressure(status))
    chttp_server_connection_close(connection);
  return status;
}

static int chttp_server_h1_input(chttp_server_connection *connection, const void *data, size_t size,
                                 unsigned int *http_status) {
  size_t consumed = 0u;
  int status;
  if (size == 0u) return SALTS_OK;
  if (connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_OPEN)
    return chttp_server_websocket_input(connection, data, size);
  status =
      chttp_server_parser_execute_consumed(&connection->parser, data, size, &consumed, http_status);
  if (status != SALTS_OK) return status;
  if (consumed < size) {
    const size_t remaining = size - consumed;
    if (connection->close_after_write) return SALTS_OK;
    const chttp_server_deferred_state deferred_state = chttp_server_deferred_token_state(
        atomic_load_explicit(&connection->deferred_token, memory_order_acquire));
    if (connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_HANDSHAKE &&
        deferred_state == CHTTP_SERVER_DEFERRED_IDLE &&
        !connection->retained_response_paused)
      return SALTS_EPROTO;
    status = chttp_server_buffer_grow(connection->server, &connection->websocket_upgrade_input,
                                      &connection->websocket_upgrade_input_capacity, remaining,
                                      connection->server->config.network.receive_buffer_bytes, 0u);
    if (status != SALTS_OK) return status;
    memmove(connection->websocket_upgrade_input, (const unsigned char *)data + consumed, remaining);
    connection->websocket_upgrade_input_size = remaining;
  }
  return SALTS_OK;
}

static int chttp_server_protocol_input(chttp_server_connection *connection, const void *data,
                                       size_t size, unsigned int *http_status) {
  const unsigned char *bytes = (const unsigned char *)data;
  size_t offset = 0u;
  int status;
  if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_1_1)
    return chttp_server_h1_input(connection, data, size, http_status);
  if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_2)
    return chttp_h2_server_connection_receive(connection->h2, data, size);
  while (offset < size && connection->protocol_prefix_size < sizeof(CHTTP_SERVER_H2_PREFACE) - 1u) {
    const size_t prefix_index = connection->protocol_prefix_size;
    const unsigned char byte = bytes[offset++];
    connection->protocol_prefix[prefix_index] = byte;
    ++connection->protocol_prefix_size;
    if (byte != CHTTP_SERVER_H2_PREFACE[prefix_index]) {
      connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_1_1;
      status = chttp_server_h1_input(connection, connection->protocol_prefix,
                                     connection->protocol_prefix_size, http_status);
      if (status == SALTS_OK && offset < size)
        status = chttp_server_h1_input(connection, bytes + offset, size - offset, http_status);
      return status;
    }
  }
  if (connection->protocol_prefix_size != sizeof(CHTTP_SERVER_H2_PREFACE) - 1u) return SALTS_OK;
  connection->wire_protocol = CHTTP_SERVER_WIRE_HTTP_2;
  status = chttp_h2_server_connection_receive(connection->h2, connection->protocol_prefix,
                                              connection->protocol_prefix_size);
  if (status == SALTS_OK && offset < size)
    status = chttp_h2_server_connection_receive(connection->h2, bytes + offset, size - offset);
  return status;
}

static void chttp_server_on_receive(void *user, cnet_connection handle,
                                    const cnet_receive_view *view) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  unsigned int http_status = 0u;
  int status;
  if (connection != NULL && connection->close_after_write) return;
  if (!chttp_server_connection_matches(connection, handle) || view == NULL ||
      view->kind != CNET_MESSAGE_BYTES ||
      (connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_OPEN &&
       connection->wire_protocol != CHTTP_SERVER_WIRE_HTTP_2 && connection->writing)) {
    chttp_server_connection_close(connection);
    return;
  }
  if (connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_OPEN) {
    status = chttp_server_websocket_input(connection, view->data, view->size);
    if (status != SALTS_OK) {
      chttp_server_stats_protocol_error(connection->server);
      connection->close_after_write = true;
      if (!connection->writing && connection->outbound_size == 0u)
        chttp_server_connection_close(connection);
    } else if (!connection->writing && connection->outbound_size == 0u) {
      (void)chttp_server_send_pending(connection);
    }
    return;
  }
  if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_2 &&
      chttp_h2_server_connection_draining(connection->h2))
    connection->h2_close_after_ms = 0u;
  if (chttp_server_deadline_expired(&connection->request_state)) status = SALTS_ETIMEDOUT;
  else {
    if (view->size != 0 && connection->request_state.deadline_ms == 0 &&
        !connection->request_state.admission_complete)
      chttp_server_on_message_begin(connection);
    status = chttp_server_protocol_input(connection, view->data, view->size, &http_status);
  }
  if (status == SALTS_ETIMEDOUT) {
    chttp_server_request_body_close(&connection->request_state, SALTS_ETIMEDOUT);
    connection->request_state.deadline_ms = 0;
    connection->pending_action = CHTTP_SERVER_PENDING_CLOSE;
    chttp_server_connection_close(connection);
    return;
  }
  if (status != SALTS_OK) {
    if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_2) {
      chttp_server_stats_protocol_error(connection->server);
      (void)chttp_h2_server_connection_flush(connection->h2);
    } else {
      if (http_status == 0u) http_status = 500u;
      if (status == SALTS_EPROTO) chttp_server_stats_protocol_error(connection->server);
      else chttp_server_stats_handler_error(connection->server);
      if (chttp_server_connection_reserve_outbound(
              connection, connection->server->max_response_wire_bytes) == SALTS_OK &&
          chttp_server_error_serialize(http_status, connection->outbound,
                                       connection->outbound_capacity,
                                       &connection->outbound_size) == SALTS_OK)
        chttp_server_stats_response(connection->server);
    }
    connection->close_after_write = true;
  }
  (void)chttp_server_send_pending(connection);
}

static int chttp_server_response_stream_finish(chttp_server_connection *connection, int status) {
  chttp_server_response_builder *builder = &connection->request_state.response_builder;
  chttp_server_response_builder_close_source(builder, status);
  connection->response_streaming = false;
  if (status == SALTS_OK) connection->close_after_write = connection->response_close_after_stream;
  return status;
}

static int chttp_server_response_stream_next(chttp_server_connection *connection) {
  static const char final_chunk[] = "0\r\n\r\n";
  chttp_server_response_builder *builder;
  chttp_body_source *source;
  unsigned char *body_output;
  size_t capacity;
  size_t size = 0u;
  size_t remaining = 0u;
  int status;
  if (connection == NULL || !connection->response_streaming || connection->outbound_size != 0u)
    return SALTS_EINVAL;
  status = chttp_server_connection_reserve_outbound(
      connection, connection->server->config.stream_chunk_bytes +
                      CHTTP_SERVER_CHUNK_PREFIX_RESERVE + CHTTP_SERVER_CHUNK_TRAILER_BYTES);
  if (status != SALTS_OK) return chttp_server_response_stream_finish(connection, status);
  builder = &connection->request_state.response_builder;
  if (!builder->source_enabled || builder->body_source.read == NULL)
    return chttp_server_response_stream_finish(connection, SALTS_EPROTO);
  source = &builder->body_source;
  capacity = connection->outbound_capacity;
  body_output = connection->outbound;
  if (connection->response_source_chunked) {
    if (capacity <= CHTTP_SERVER_CHUNK_PREFIX_RESERVE + CHTTP_SERVER_CHUNK_TRAILER_BYTES)
      return chttp_server_response_stream_finish(connection, SALTS_EMSGSIZE);
    body_output += CHTTP_SERVER_CHUNK_PREFIX_RESERVE;
    capacity -= CHTTP_SERVER_CHUNK_PREFIX_RESERVE + CHTTP_SERVER_CHUNK_TRAILER_BYTES;
  }
  if (capacity > connection->server->config.stream_chunk_bytes)
    capacity = connection->server->config.stream_chunk_bytes;
  if (source->content_length_known) {
    if (builder->source_transferred > source->content_length)
      return chttp_server_response_stream_finish(connection, SALTS_EPROTO);
    remaining = source->content_length - builder->source_transferred;
    if (remaining != 0u && capacity > remaining) capacity = remaining;
  } else {
    if (builder->source_transferred > builder->source_capacity)
      return chttp_server_response_stream_finish(connection, SALTS_EMSGSIZE);
    remaining = builder->source_capacity - builder->source_transferred;
    if (remaining != 0u && capacity > remaining) capacity = remaining;
  }
  if (capacity == 0u) capacity = 1u;
  if (builder->file_transfer != NULL) {
    const chttp_file_source_result result =
        chttp_file_transfer_read(builder->file_transfer, body_output, capacity, &size);
    if (result == CHTTP_FILE_SOURCE_WAIT) return SALTS_OK;
    if (result == CHTTP_FILE_SOURCE_ERROR) {
      status = chttp_file_transfer_status(builder->file_transfer, NULL);
      return chttp_server_response_stream_finish(connection,
                                                 status == SALTS_OK ? SALTS_EIO : status);
    }
    if (result == CHTTP_FILE_SOURCE_EOF) size = 0u;
  } else {
    status = source->read(source->user, body_output, capacity, &size);
    if (status != SALTS_OK) return chttp_server_response_stream_finish(connection, status);
  }
  if (size > capacity) return chttp_server_response_stream_finish(connection, SALTS_EPROTO);
  if (source->content_length_known) {
    if (remaining == 0u)
      return size == 0u ? chttp_server_response_stream_finish(connection, SALTS_OK)
                        : chttp_server_response_stream_finish(connection, SALTS_EPROTO);
    if (size == 0u) return chttp_server_response_stream_finish(connection, SALTS_EPROTO);
  } else if (remaining == 0u && size != 0u)
    return chttp_server_response_stream_finish(connection, SALTS_EMSGSIZE);
  if (size == 0u) {
    if (connection->response_source_chunked) {
      memcpy(connection->outbound, final_chunk, sizeof(final_chunk) - 1u);
      connection->outbound_size = sizeof(final_chunk) - 1u;
    }
    return chttp_server_response_stream_finish(connection, SALTS_OK);
  }
  builder->source_transferred += size;
  if (connection->response_source_chunked) {
    char prefix[CHTTP_SERVER_CHUNK_PREFIX_RESERVE];
    const int prefix_size = snprintf(prefix, sizeof(prefix), "%zx\r\n", size);
    if (prefix_size <= 0 || (size_t)prefix_size >= sizeof(prefix))
      return chttp_server_response_stream_finish(connection, SALTS_EMSGSIZE);
    memmove(connection->outbound + (size_t)prefix_size, body_output, size);
    memcpy(connection->outbound, prefix, (size_t)prefix_size);
    memcpy(connection->outbound + (size_t)prefix_size + size, "\r\n",
           CHTTP_SERVER_CHUNK_TRAILER_BYTES);
    connection->outbound_size = (size_t)prefix_size + size + CHTTP_SERVER_CHUNK_TRAILER_BYTES;
  } else connection->outbound_size = size;
  return SALTS_OK;
}

static void chttp_server_h1_file_ready(void *user) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  chttp_server_response_builder *builder;
  int status;
  if (connection == NULL || !connection->active || !connection->response_streaming ||
      connection->writing || connection->outbound_size != 0u)
    return;
  builder = &connection->request_state.response_builder;
  if (builder->file_transfer == NULL || !chttp_file_transfer_ready(builder->file_transfer)) return;
  status = chttp_server_response_stream_next(connection);
  if (status != SALTS_OK) {
    chttp_server_connection_close(connection);
    return;
  }
  if (connection->outbound_size != 0u) (void)chttp_server_send_pending(connection);
  else if (connection->close_after_write) chttp_server_connection_close(connection);
  else if (!connection->response_streaming) (void)chttp_server_send_pending(connection);
}

static int chttp_server_h1_input_resume(chttp_server_connection *connection) {
  unsigned int http_status = 0u;
  size_t size;
  int status;
  if (connection == NULL) return SALTS_EINVAL;
  status = chttp_server_parser_resume(&connection->parser);
  if (status != SALTS_OK) return status;
  size = connection->websocket_upgrade_input_size;
  connection->websocket_upgrade_input_size = 0u;
  if (size == 0u) return SALTS_OK;
  status =
      chttp_server_h1_input(connection, connection->websocket_upgrade_input, size, &http_status);
  if (connection->websocket_upgrade_input_size == 0u) {
    chttp_server_buffer_release(connection->server, connection->websocket_upgrade_input,
                                connection->websocket_upgrade_input_capacity);
    connection->websocket_upgrade_input = NULL;
    connection->websocket_upgrade_input_capacity = 0u;
  }
  return status;
}

static void chttp_server_on_send(void *user, cnet_connection handle, size_t size) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  const bool resume_deferred = connection != NULL && connection->deferred_response_writing;
  const bool resume_retained = connection != NULL && connection->retained_response_paused;
  size_t expected_size;
  int status = SALTS_OK;
  if (!chttp_server_connection_matches(connection, handle) || !connection->writing) {
    chttp_server_connection_close(connection);
    return;
  }
  expected_size = connection->outbound_size;
  if (connection->retained_response_sg) {
    if (connection->retained_response_sg_body_size > SIZE_MAX - expected_size) {
      chttp_server_connection_close(connection);
      return;
    }
    expected_size += connection->retained_response_sg_body_size;
  }
  if (size != expected_size) {
    chttp_server_connection_close(connection);
    return;
  }
  connection->writing = false;
  connection->outbound_size = 0u;
  connection->retained_response_sg = false;
  connection->retained_response_sg_body_size = 0u;
  connection->deferred_response_writing = false;
  connection->retained_response_paused = false;
  if (resume_retained)
    chttp_server_response_builder_release_retained_body(
        &connection->request_state.response_builder);
  if (connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_NONE) {
    chttp_server_websocket_profile_send_complete(connection);
    status = chttp_server_websocket_send_complete(connection);
  }
  if (connection->response_streaming) status = chttp_server_response_stream_next(connection);
  if (connection->outbound_size == 0u && !connection->response_streaming &&
      connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE &&
      connection->wire_protocol != CHTTP_SERVER_WIRE_HTTP_2)
    chttp_server_connection_release_outbound(connection);
  if (status == SALTS_OK && (resume_deferred || resume_retained) &&
      !connection->close_after_write)
    status = chttp_server_h1_input_resume(connection);
  if (status != SALTS_OK) {
    chttp_server_connection_close(connection);
    return;
  }
  if (connection->outbound_size != 0u) (void)chttp_server_send_pending(connection);
  else if (connection->close_after_write) chttp_server_connection_close(connection);
  else if (!connection->response_streaming) (void)chttp_server_send_pending(connection);
}

static int chttp_server_on_headers(void *user, const chttp_server_request_view *request,
                                   chttp_server_parser_headers_action *out_action) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  chttp_server_request_state *state;
  chttp_server_request_view routed_request;
  chttp_server_response_builder *builder;
  int status;

  if (connection == NULL || request == NULL || out_action == NULL) return SALTS_EINVAL;
  *out_action = CHTTP_SERVER_HEADERS_CONTINUE;
  state = &connection->request_state;
  if (chttp_server_deadline_expired(state)) return SALTS_ETIMEDOUT;
  chttp_server_deadline_start(state, connection->server->deadlines.body_ms);
  chttp_server_request_admission_clear(state);
  chttp_server_response_builder_reset(&state->response_builder);

  routed_request = *request;
  chttp_server_request_enrich(connection, &routed_request);
  status = chttp_server_request_admit(state, &routed_request, routed_request.method);
  if (status == SALTS_OK) return SALTS_OK;
  if (status != SALTS_EPERM) return status;

  routed_request.protocol_keep_alive = 0;
  builder = &state->response_builder;
  builder->request = &routed_request;
  state->deadline_ms = 0;
  status = chttp_server_request_rejection_response(state);
  if (status == SALTS_OK)
    status = chttp_server_connection_reserve_outbound(
        connection, connection->outbound_size + connection->server->max_response_wire_bytes);
  if (status == SALTS_OK)
    status =
        chttp_server_response_serialize(builder, &routed_request, connection->outbound,
                                        connection->outbound_capacity, &connection->outbound_size);
  builder->request = NULL;
  if (status != SALTS_OK) return status;

  chttp_server_stats_response(connection->server);
  connection->close_after_write = true;
  *out_action = CHTTP_SERVER_HEADERS_STOP;
  return SALTS_OK;
}

static int chttp_server_on_continue(void *user) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  int status;
  if (connection == NULL) return SALTS_EINVAL;
  status = chttp_server_connection_reserve_outbound(
      connection, connection->outbound_size + sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u);
  if (status != SALTS_OK) return status;
  if (connection->outbound_size > connection->outbound_capacity ||
      sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u >
          connection->outbound_capacity - connection->outbound_size)
    return SALTS_EMSGSIZE;
  memcpy(connection->outbound + connection->outbound_size, CHTTP_SERVER_CONTINUE_RESPONSE,
         sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u);
  connection->outbound_size += sizeof(CHTTP_SERVER_CONTINUE_RESPONSE) - 1u;
  return SALTS_OK;
}

int chttp_server_request_body_open(chttp_server_request_state *state,
                                   const chttp_server_request_view *request,
                                   chttp_body_sink *out_sink) {
  chttp_server_request_view routed_request;
  chttp_server_route_record *route;
  chttp_server_impl *previous_callback_server;
  int status;

  if (state == NULL || state->server == NULL || request == NULL || out_sink == NULL)
    return SALTS_EINVAL;
  *out_sink = (chttp_body_sink){0};
  if (chttp_server_deadline_expired(state)) return SALTS_ETIMEDOUT;
  if (state->body_sink_open) return SALTS_EBUSY;
  if (!state->admission_complete || state->admission_rejected) return SALTS_EPERM;

  state->body_route = NULL;
  state->body_sink = (chttp_body_sink){0};
  state->body_sink_user = NULL;
  state->body_was_streamed = false;
  route = state->admitted_route;
  if (route == NULL || route->body_open == NULL) return SALTS_OK;

  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = NULL;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
  routed_request.body_sink_user = NULL;
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = state->server;
  status = route->body_open(route->user, &routed_request, out_sink);
  chttp_active_callback_server = previous_callback_server;
  if (status != SALTS_OK) {
    *out_sink = (chttp_body_sink){0};
    return status;
  }
  if (out_sink->write == NULL) return SALTS_EINVAL;

  state->body_route = route;
  state->body_sink = *out_sink;
  state->body_sink_user = out_sink->user;
  state->body_sink_open = true;
  state->body_was_streamed = true;
  return SALTS_OK;
}

int chttp_server_request_body_write(chttp_server_request_state *state, const void *data,
                                    size_t size) {
  if (state != NULL && chttp_server_deadline_expired(state)) return SALTS_ETIMEDOUT;
  if (state == NULL || !state->body_sink_open || state->body_sink.write == NULL ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  return size == 0u ? SALTS_OK : state->body_sink.write(state->body_sink.user, data, size);
}

void chttp_server_request_body_close(chttp_server_request_state *state, int status) {
  chttp_server_route_record *route;
  chttp_body_sink sink;
  chttp_server_impl *previous_callback_server;
  if (state == NULL || !state->body_sink_open) return;
  if (status == SALTS_OK && chttp_server_deadline_expired(state)) status = SALTS_ETIMEDOUT;
  route = state->body_route;
  sink = state->body_sink;
  state->body_sink_open = false;
  state->body_route = NULL;
  state->body_sink = (chttp_body_sink){0};
  if (route == NULL || route->body_close == NULL) return;
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = state->server;
  route->body_close(route->user, &sink, status);
  chttp_active_callback_server = previous_callback_server;
}

bool chttp_server_request_body_streaming(const chttp_server_request_state *state) {
  return state != NULL && state->body_was_streamed;
}

static int chttp_server_on_body_open(void *user, const chttp_server_request_view *request,
                                     chttp_body_sink *out_sink) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  if (connection == NULL) return SALTS_EINVAL;
  return chttp_server_request_body_open(&connection->request_state, request, out_sink);
}

static void chttp_server_on_body_close(void *user, chttp_body_sink *sink, int status) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  (void)sink;
  if (connection != NULL) chttp_server_request_body_close(&connection->request_state, status);
}

int chttp_server_dispatch_request(chttp_server_request_state *state,
                                  const chttp_server_request_view *request) {
  chttp_server_impl *server;
  chttp_server_request_view routed_request;
  chttp_server_route_record *route;
  chttp_server_chain chain;
  chttp_server_impl *previous_callback_server;
  int status;

  if (state == NULL || state->server == NULL || request == NULL) return SALTS_EINVAL;
  if (!state->admission_complete || state->admission_rejected) return SALTS_EPERM;
  if (chttp_server_deadline_expired(state)) return SALTS_ETIMEDOUT;
  chttp_server_deadline_start(state, state->server->deadlines.handler_ms);

  server = state->server;
  route = state->admitted_route;
  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = server->config.session_capacity == 0u ? NULL : &state->session;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
  routed_request.body_sink_user =
      state->body_was_streamed ? state->body_sink_user : NULL;
  chttp_server_response_builder_reset(&state->response_builder);
  chttp_session_request_begin(state, &routed_request);
  chain = (chttp_server_chain){.server = server,
                               .request_state = state,
                               .request = &routed_request,
                               .response = &state->response,
                               .route = route,
                               .fallback_status = state->admitted_fallback_status,
                               .allowed_methods = state->admitted_allowed_methods};
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = server;
  status = chttp_server_chain_run(&chain);
  chttp_active_callback_server = previous_callback_server;
  if (status == SALTS_OK && state->response_builder.deferred) return SALTS_OK;
  if (chttp_server_deadline_expired(state)) {
    state->deadline_ms = 0;
    chttp_session_request_abort(state);
    chttp_server_response_builder_close_source(&state->response_builder, SALTS_ETIMEDOUT);
    chttp_server_response_builder_reset(&state->response_builder);
    return SALTS_ETIMEDOUT;
  }
  state->deadline_ms = 0;
  if (status == SALTS_OK && !state->response_builder.replied)
    status = chttp_server_reply(&state->response, 204u, NULL, NULL, 0u);
  if (status == SALTS_OK) status = chttp_session_request_finish(state);
  if (status != SALTS_OK) {
    chttp_session_request_abort(state);
    chttp_server_stats_handler_error(server);
    chttp_server_response_builder_reset(&state->response_builder);
    status = chttp_server_reply(&state->response, 500u, "text/plain", "Internal Server Error", 21u);
  }
  return status;
}

static int chttp_server_on_request(void *user, const chttp_server_request_view *request) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  chttp_server_request_view enriched_request;
  chttp_server_response_builder *builder;
  int status;
  if (connection == NULL || request == NULL || connection->close_after_write)
    return SALTS_ESHUTDOWN;
  if (connection->outbound_size > connection->server->config.network.max_send_bytes ||
      connection->server->max_response_wire_bytes >
          connection->server->config.network.max_send_bytes - connection->outbound_size)
    return SALTS_ENOBUFS;
  enriched_request = *request;
  chttp_server_request_enrich(connection, &enriched_request);
  request = &enriched_request;
  builder = &connection->request_state.response_builder;
  builder->request = request;
  status = chttp_server_dispatch_request(&connection->request_state, request);
  builder->request = NULL;
  if (status == SALTS_OK && builder->deferred) return CHTTP_SERVER_REQUEST_DEFERRED;
  if (status == SALTS_OK) {
    status = chttp_server_connection_reserve_outbound(
        connection, connection->outbound_size + connection->server->max_response_wire_bytes);
  }
  if (status == SALTS_OK) {
    const bool retained_sg =
        builder->retained_body != NULL &&
        builder->body_size != 0u &&
        request->method != CHTTP_METHOD_HEAD &&
        connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_1_1 &&
        !connection->server->tls_initialized;
    status = retained_sg
                 ? chttp_server_response_serialize_headers(
                       builder, request, connection->outbound,
                       connection->outbound_capacity,
                       &connection->outbound_size)
                 : chttp_server_response_serialize(
                       builder, request, connection->outbound,
                       connection->outbound_capacity,
                       &connection->outbound_size);
    if (status == SALTS_OK && retained_sg) {
      connection->retained_response_sg = true;
      connection->retained_response_sg_body_size = builder->body_size;
    }
    if (status != SALTS_OK) {
      connection->retained_response_sg = false;
      connection->retained_response_sg_body_size = 0u;
      chttp_session_request_abort(&connection->request_state);
    }
  }
  if (status == SALTS_OK) {
    chttp_server_stats_response(connection->server);
    if (builder->source_enabled && request->method != CHTTP_METHOD_HEAD) {
      connection->response_streaming = true;
      connection->response_source_chunked = !builder->body_source.content_length_known &&
                                            request->http_major == 1u && request->http_minor == 1u;
      connection->response_close_after_stream =
          !request->protocol_keep_alive ||
          (!builder->body_source.content_length_known && !connection->response_source_chunked);
      if (builder->file_transfer != NULL)
        chttp_file_transfer_set_ready(builder->file_transfer, chttp_server_h1_file_ready,
                                      connection);
    } else {
      if (builder->source_enabled) chttp_server_response_builder_close_source(builder, SALTS_OK);
      if (!request->protocol_keep_alive) connection->close_after_write = true;
      if (builder->retained_body != NULL &&
          connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_1_1) {
        connection->retained_response_paused = true;
        return CHTTP_SERVER_REQUEST_DEFERRED;
      }
    }
  }
  return status;
}

/* HTTP deferred requests retain their context after transport terminal. The
 * owner releases the extra hold only when HTTP/1 and HTTP/2 have both finished. */
static int chttp_server_manager_progress(chttp_server_owner_lane *owner) {
  size_t work;
  const size_t end = chttp_server_owner_connection_end(owner);
  for (size_t i = owner->connection_begin; i < end; ++i) {
    chttp_server_connection *connection = &owner->server->connections[i];
    if (connection->managed.slot == 0u || connection->active ||
        chttp_server_deferred_token_state(atomic_load_explicit(
            &connection->deferred_token, memory_order_acquire)) != CHTTP_SERVER_DEFERRED_IDLE ||
        chttp_h2_server_connection_has_deferred(connection->h2)) continue;
    const int status = cnet_manager_release_context(&owner->manager, connection->managed);
    if (status != SALTS_OK) return status;
    connection->managed = (cnet_managed_connection){0};
  }
  return cnet_manager_advance(&owner->manager, owner->connection_count, &work);
}

static chttp_server_connection *chttp_server_free_connection(
    chttp_server_owner_lane *owner) {
  chttp_server_impl *server;
  size_t index;
  size_t end;
  if (owner == NULL || owner->server == NULL || owner->network == NULL)
    return NULL;
  server = owner->server;
  end = chttp_server_owner_connection_end(owner);
  if (owner->connection_begin > server->config.network.connection_capacity ||
      end < owner->connection_begin ||
      end > server->config.network.connection_capacity)
    return NULL;
  for (index = owner->connection_begin; index < end; ++index)
    if (!server->connections[index].active && server->connections[index].managed.slot == 0u &&
        chttp_server_deferred_token_state(atomic_load_explicit(
            &server->connections[index].deferred_token, memory_order_acquire)) ==
            CHTTP_SERVER_DEFERRED_IDLE &&
        !chttp_h2_server_connection_has_deferred(server->connections[index].h2))
      return &server->connections[index];
  return NULL;
}

static int chttp_server_owner_admission_enqueue(
    chttp_server_owner_lane *owner, cnet_handoff_ticket ticket,
    cnet_accepted_stream *accepted) {
  if (chttp_server_owner_runtime_state_get(owner) !=
      CHTTP_SERVER_OWNER_RUNTIME_READY)
    return SALTS_ESHUTDOWN;
  return cnet_handoff_publish(&owner->handoff, ticket, accepted);
}

static int chttp_server_owner_admission_cancel(
    chttp_server_owner_lane *owner) {
  cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
  cnet_handoff_ticket ticket;
  int first_status = SALTS_OK;
  int status = cnet_handoff_seal(&owner->handoff);
  if (status != SALTS_OK) return status;
  while ((status = cnet_handoff_take(&owner->handoff, &ticket, &accepted)) == SALTS_OK) {
    const int close_status = cnet_accepted_stream_close(&accepted);
    const int release_status = cnet_handoff_release(&owner->handoff, ticket);
    if (first_status == SALTS_OK && close_status != SALTS_OK) first_status = close_status;
    if (first_status == SALTS_OK && release_status != SALTS_OK) first_status = release_status;
  }
  return first_status != SALTS_OK ? first_status : status == SALTS_ENOENT ? SALTS_OK : status;
}

static void chttp_server_stats_rejected_connection(chttp_server_impl *server) {
  cmeta_mutex_lock(&server->mutex);
  ++server->stats.rejected_connections;
  cmeta_mutex_unlock(&server->mutex);
}

static int chttp_server_admission_owner(
    chttp_server_impl *server, cnet_handoff_ticket *ticket,
    chttp_server_owner_lane **out_owner) {
  size_t offset;
  *out_owner = NULL;
  if (server == NULL || server->owner_count == 0u) return SALTS_EINVAL;
  for (offset = 0u; offset < server->owner_count; ++offset) {
    const size_t index =
        (server->admission_cursor + offset) % server->owner_count;
    chttp_server_owner_lane *owner = chttp_server_owner_at(server, index);
    if (owner == NULL ||
        chttp_server_owner_runtime_state_get(owner) !=
            CHTTP_SERVER_OWNER_RUNTIME_READY)
      continue;
    const int status = cnet_handoff_reserve(&owner->handoff, ticket);
    if (status == SALTS_ENOBUFS || status == SALTS_ESHUTDOWN) continue;
    if (status != SALTS_OK) return status;
    server->admission_cursor = (index + 1u) % server->owner_count;
    *out_owner = owner;
    return SALTS_OK;
  }
  return SALTS_ENOBUFS;
}

static int chttp_server_listener_progress(chttp_server_impl *server) {
  size_t attempts;
  if (server == NULL || !server->listener_initialized) return SALTS_EINVAL;
  for (attempts = 0u;
       attempts < server->config.network.connection_capacity;
       ++attempts) {
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    cnet_handoff_ticket ticket = {0};
    chttp_server_owner_lane *owner;
    cnet_client *network;
    int status;
    if (chttp_server_should_stop(server)) return SALTS_OK;
    status = cnet_listener_accept_detached(&server->listener, &accepted);
    if (status == SALTS_ETIMEDOUT) return SALTS_OK;
    if (status == SALTS_ENOBUFS) {
      chttp_server_stats_rejected_connection(server);
      return SALTS_OK;
    }
    if (status != SALTS_OK) return status;

    status = chttp_server_admission_owner(server, &ticket, &owner);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      if (status != SALTS_ENOBUFS) return status;
      chttp_server_stats_rejected_connection(server);
      continue;
    }
    status = chttp_server_owner_admission_enqueue(owner, ticket, &accepted);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      (void)cnet_handoff_release(&owner->handoff, ticket);
      if (status != SALTS_ESHUTDOWN)
        chttp_server_stats_rejected_connection(server);
      if (status == SALTS_ENOBUFS) continue;
      if (status == SALTS_ESHUTDOWN && chttp_server_should_stop(server))
        return SALTS_OK;
      return status;
    }
    network = chttp_server_owner_network(owner);
    if (network == NULL) return SALTS_EPROTO;
    {
      status = cnet_client_wake(network);
      /* Publication already transferred the descriptor and credit. On wake
       * failure stop admission and let the owner cancel its inbox after the
       * listener_done barrier; never close/release the published item here. */
      if (status != SALTS_OK) return status;
    }
  }
  return SALTS_OK;
}

static void chttp_server_connection_activate(
    chttp_server_impl *server, chttp_server_owner_lane *owner,
    chttp_server_connection *connection, cnet_connection handle,
    const cnet_stream_peer *peer) {
  connection->owner = owner;
  ++connection->server_generation;
  if (connection->server_generation == 0u) connection->server_generation = 1u;
  connection->handle = handle;
  connection->peer = *peer;
  connection->peer_certificate_sha256[0] = '\0';
  connection->active = true;
  connection->connected = false;
  connection->writing = false;
  connection->close_after_write = false;
  connection->response_streaming = false;
  connection->response_source_chunked = false;
  connection->response_close_after_stream = false;
  connection->deferred_disconnected = false;
  connection->deferred_response_writing = false;
  connection->retained_response_paused = false;
  connection->retained_response_sg = false;
  connection->retained_response_sg_body_size = 0u;
  connection->pending_action = CHTTP_SERVER_PENDING_NONE;
  connection->outbound_size = 0u;
  connection->h2_close_after_ms = 0u;
  connection->protocol_prefix_size = 0u;
  connection->wire_protocol =
      server->config.enable_http2 ? CHTTP_SERVER_WIRE_UNKNOWN
                                  : CHTTP_SERVER_WIRE_HTTP_1_1;
  chttp_server_stats_connection_open(server);
}

static int chttp_server_admission_progress(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  cnet_client *network;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  network = chttp_server_owner_network(owner);
  if (network == NULL) return SALTS_EINVAL;
  {
    const int status = chttp_server_manager_progress(owner);
    if (status != SALTS_OK) return status;
  }
  for (;;) {
    chttp_server_connection *connection = chttp_server_free_connection(owner);
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    cnet_handoff_ticket ticket = {0};
    cnet_observer observer;
    cnet_connection handle = {0};
    cnet_stream_peer peer;
    int status;
    if (connection == NULL) return SALTS_OK;
    status = cnet_handoff_take(&owner->handoff, &ticket, &accepted);
    if (status == SALTS_ENOENT) return SALTS_OK;
    if (status != SALTS_OK) return status;

    chttp_server_websocket_reset(connection);
    chttp_server_request_state_reset(&connection->request_state);
    status = chttp_server_parser_reset(&connection->parser);
    if (status == SALTS_OK && server->config.enable_http2)
      status = chttp_h2_server_connection_prepare(connection->h2);
    if (status != SALTS_OK) {
      (void)cnet_accepted_stream_close(&accepted);
      (void)cnet_handoff_release(&owner->handoff, ticket);
      chttp_server_stats_rejected_connection(server);
      return status;
    }

    peer = accepted.peer;
    observer = (cnet_observer){.on_state = chttp_server_on_state,
                               .on_receive = chttp_server_on_receive,
                               .on_send = chttp_server_on_send,
                               .user = connection};
    const cnet_manager_attachment attachment = {.observer = observer, .hold_context = true};
    status = cnet_manager_reserve(&owner->manager, &attachment, &connection->managed);
    if (status == SALTS_OK)
      status = cnet_manager_adopt(&owner->manager, connection->managed, &accepted,
                                 server->tls_initialized ? &server->tls_server : NULL, &handle);
    else
      (void)cnet_accepted_stream_close(&accepted);
    if (status != SALTS_OK) {
      (void)cnet_handoff_release(&owner->handoff, ticket);
      chttp_server_stats_rejected_connection(server);
      const int recycle_status = chttp_server_manager_progress(owner);
      if (recycle_status != SALTS_OK) return recycle_status;
      if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) continue;
      return status;
    }
    chttp_server_connection_activate(
        server, owner, connection, handle, &peer);
    connection->owner_ticket = ticket;
  }
}

static int chttp_server_deadlines_progress(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t begin;
  size_t end;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  begin = owner->connection_begin;
  end = chttp_server_owner_connection_end(owner);
  if (begin > server->config.network.connection_capacity || end < begin ||
      end > server->config.network.connection_capacity)
    return SALTS_EINVAL;
  /* O(connections + active H2 stream slots); timers use existing owner-held request storage. */
  if (server->deadlines.headers_ms == 0 && server->deadlines.body_ms == 0 &&
      server->deadlines.handler_ms == 0) return SALTS_OK;
  for (size_t index = begin; index < end; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    if (!connection->active || connection->close_after_write) continue;
    if (connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_NONE) {
      connection->request_state.deadline_ms = 0;
      continue;
    }
    chttp_h2_server_connection_deadlines(connection->h2);
    if (!chttp_server_deadline_expired(&connection->request_state)) continue;
    uint_fast64_t token = atomic_load_explicit(&connection->deferred_token, memory_order_acquire);
    connection->request_state.deadline_ms = 0;
    if (chttp_server_deferred_token_state(token) != CHTTP_SERVER_DEFERRED_IDLE) {
      if (chttp_server_deferred_token_state(token) == CHTTP_SERVER_DEFERRED_PENDING)
        (void)atomic_compare_exchange_strong_explicit(&connection->deferred_token, &token,
            chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                CHTTP_SERVER_DEFERRED_CANCELED), memory_order_acq_rel, memory_order_acquire);
      continue;
    }
    chttp_server_request_body_close(&connection->request_state, SALTS_ETIMEDOUT);
    chttp_session_request_abort(&connection->request_state);
    connection->pending_action = CHTTP_SERVER_PENDING_CLOSE;
    chttp_server_connection_close(connection);
  }
  return SALTS_OK;
}

static int chttp_server_deferred_progress(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t index;
  size_t begin;
  size_t end;
  int status;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  begin = owner->connection_begin;
  end = chttp_server_owner_connection_end(owner);
  if (begin > server->config.network.connection_capacity || end < begin ||
      end > server->config.network.connection_capacity)
    return SALTS_EINVAL;
  status = chttp_server_deadlines_progress(server, owner);
  if (status != SALTS_OK) return status;
  for (index = begin; index < end; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    chttp_server_response_builder *builder = &connection->deferred_builder;
    const uint_fast64_t deferred_token =
        atomic_load_explicit(&connection->deferred_token, memory_order_acquire);
    const chttp_server_deferred_state deferred_state =
        chttp_server_deferred_token_state(deferred_token);
    status = chttp_h2_server_connection_deferred_progress(connection->h2);
    if (status != SALTS_OK) return status;
    if (deferred_state == CHTTP_SERVER_DEFERRED_CANCELED) {
      chttp_session_request_abort(&connection->request_state);
      chttp_server_request_state_reset(&connection->request_state);
      chttp_server_response_builder_reset(builder);
      (void)chttp_server_parser_reset(&connection->parser);
      connection->deferred_disconnected = false;
      atomic_store_explicit(
          &connection->deferred_token,
          chttp_server_deferred_token(chttp_server_deferred_token_generation(deferred_token),
                                      CHTTP_SERVER_DEFERRED_IDLE),
          memory_order_release);
      if (connection->active) chttp_server_connection_close(connection);
      continue;
    }
    if (deferred_state != CHTTP_SERVER_DEFERRED_READY) continue;
    connection->request_state.deadline_ms = 0;
    if (connection->deferred_disconnected || !connection->active || !connection->connected) {
      chttp_session_request_abort(&connection->request_state);
      chttp_server_request_state_reset(&connection->request_state);
      chttp_server_response_builder_reset(builder);
      (void)chttp_server_parser_reset(&connection->parser);
      connection->deferred_disconnected = false;
      atomic_store_explicit(
          &connection->deferred_token,
          chttp_server_deferred_token(chttp_server_deferred_token_generation(deferred_token),
                                      CHTTP_SERVER_DEFERRED_IDLE),
          memory_order_release);
      continue;
    }
    status = chttp_session_request_finish(&connection->request_state);
    if (status == SALTS_OK)
      status = chttp_server_connection_reserve_outbound(
          connection, connection->outbound_size + server->max_response_wire_bytes);
    if (status == SALTS_OK)
      status = chttp_server_response_serialize(builder, &connection->deferred_target.request,
                                               connection->outbound, connection->outbound_capacity,
                                               &connection->outbound_size);
    if (status != SALTS_OK) {
      chttp_session_request_abort(&connection->request_state);
      chttp_server_stats_handler_error(server);
      chttp_server_request_state_reset(&connection->request_state);
      chttp_server_response_builder_reset(builder);
      atomic_store_explicit(
          &connection->deferred_token,
          chttp_server_deferred_token(chttp_server_deferred_token_generation(deferred_token),
                                      CHTTP_SERVER_DEFERRED_IDLE),
          memory_order_release);
      chttp_server_connection_close(connection);
      continue;
    }
    if (!connection->deferred_target.request.protocol_keep_alive)
      connection->close_after_write = true;
    connection->deferred_response_writing = true;
    chttp_server_stats_response(server);
    atomic_store_explicit(
        &connection->deferred_token,
        chttp_server_deferred_token(chttp_server_deferred_token_generation(deferred_token),
                                    CHTTP_SERVER_DEFERRED_IDLE),
        memory_order_release);
    status = chttp_server_send_pending(connection);
    if (status != SALTS_OK && !chttp_server_action_pressure(status)) return status;
  }
  return SALTS_OK;
}

static int chttp_server_retry_pending(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t offset;
  size_t begin;
  size_t count;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  begin = owner->connection_begin;
  count = owner->connection_count;
  if (count == 0u || begin > server->config.network.connection_capacity ||
      count > server->config.network.connection_capacity - begin)
    return SALTS_EINVAL;
  if (owner->pending_retry_cursor >= count) owner->pending_retry_cursor = 0u;
  for (offset = 0u; offset < count; ++offset) {
    const size_t local = (owner->pending_retry_cursor + offset) % count;
    const size_t index = begin + local;
    chttp_server_connection *connection = &server->connections[index];
    int status;
    if (!connection->active || connection->pending_action == CHTTP_SERVER_PENDING_NONE) continue;
    status = chttp_server_connection_retry(connection);
    owner->pending_retry_cursor = (local + 1u) % count;
    if (status == SALTS_ENOBUFS) return SALTS_OK;
    if (status != SALTS_OK && status != SALTS_EBUSY) return status;
  }
  return SALTS_OK;
}

static bool chttp_server_should_stop(chttp_server_impl *server) {
  bool stop;
  cmeta_mutex_lock(&server->mutex);
  stop = server->stop_requested;
  cmeta_mutex_unlock(&server->mutex);
  return stop;
}

static bool chttp_server_connections_active(const chttp_server_impl *server,
                                            const chttp_server_owner_lane *owner) {
  size_t index;
  size_t end;
  if (server == NULL || owner == NULL || owner->server != server) return false;
  end = chttp_server_owner_connection_end(owner);
  if (owner->connection_begin > server->config.network.connection_capacity ||
      end < owner->connection_begin || end > server->config.network.connection_capacity)
    return false;
  for (index = owner->connection_begin; index < end; ++index)
    if (server->connections[index].active ||
        chttp_server_deferred_token_state(atomic_load_explicit(
            &server->connections[index].deferred_token, memory_order_acquire)) !=
            CHTTP_SERVER_DEFERRED_IDLE ||
        chttp_h2_server_connection_has_deferred(server->connections[index].h2))
      return true;
  return false;
}

static int chttp_server_begin_shutdown(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t index;
  size_t end;
  int status;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  end = chttp_server_owner_connection_end(owner);
  if (owner->connection_begin > server->config.network.connection_capacity ||
      end < owner->connection_begin || end > server->config.network.connection_capacity)
    return SALTS_EINVAL;
  status = cnet_manager_seal(&owner->manager);
  if (status != SALTS_OK) return status;
  if (owner == &server->owner && server->listener_initialized) {
    status = cnet_listener_close(&server->listener);
    if (status != SALTS_OK && status != SALTS_EALREADY) return status;
  }
  status = chttp_server_owner_admission_cancel(owner);
  if (status != SALTS_OK) return status;
  for (index = owner->connection_begin; index < end; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    if (!connection->active) continue;
    if (connection->wire_protocol == CHTTP_SERVER_WIRE_HTTP_2) {
      status = chttp_h2_server_connection_begin_stop(connection->h2);
      if (status != SALTS_OK) return status;
      status = chttp_server_send_pending(connection);
      if (status != SALTS_OK && !chttp_server_action_pressure(status)) return status;
    } else {
      chttp_server_connection_close(connection);
    }
  }
  return SALTS_OK;
}

static void chttp_server_progress_shutdown(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  const uint64_t now_ms = cmeta_monotonic_ms();
  size_t index;
  size_t end;
  if (server == NULL || owner == NULL || owner->server != server) return;
  end = chttp_server_owner_connection_end(owner);
  if (owner->connection_begin > server->config.network.connection_capacity ||
      end < owner->connection_begin || end > server->config.network.connection_capacity)
    return;
  for (index = owner->connection_begin; index < end; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    if (!connection->active || connection->wire_protocol != CHTTP_SERVER_WIRE_HTTP_2 ||
        connection->h2_close_after_ms == 0u || now_ms < connection->h2_close_after_ms ||
        !chttp_h2_server_connection_stop_waiting(connection->h2))
      continue;
    connection->h2_close_after_ms = 0u;
    chttp_server_connection_close(connection);
  }
}

static void chttp_server_wake_owners(chttp_server_impl *server) {
  size_t index;
  if (server == NULL) return;
  for (index = 0u; index < server->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(server, index);
    cnet_client *network = chttp_server_owner_network(owner);
    if (network != NULL) (void)cnet_client_wake(network);
  }
}

static void chttp_server_owner_worker_finish(
    chttp_server_impl *server, chttp_server_owner_lane *owner,
    int terminal_status) {
  cmeta_mutex_lock(&server->mutex);
  owner->terminal_status = terminal_status;
  ++server->finished_owner_count;
  if (terminal_status != SALTS_OK &&
      server->stats.terminal_status == SALTS_OK)
    server->stats.terminal_status = terminal_status;
  if (server->start_called &&
      server->finished_owner_count >= server->started_owner_count &&
      server->listener_done) {
    server->stats.running = 0;
    server->stats.stopping = 0;
    server->worker_done = true;
  }
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
}

static void chttp_server_listener_worker_finish(
    chttp_server_impl *server, int terminal_status) {
  cmeta_mutex_lock(&server->mutex);
  server->listener_terminal_status = terminal_status;
  server->listener_done = true;
  if (terminal_status != SALTS_OK &&
      terminal_status != SALTS_ECANCELED &&
      server->stats.terminal_status == SALTS_OK)
    server->stats.terminal_status = terminal_status;
  if (server->start_called &&
      server->finished_owner_count >= server->started_owner_count) {
    server->stats.running = 0;
    server->stats.stopping = 0;
    server->worker_done = true;
  }
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
}

static cnet_client *chttp_server_owner_network_storage(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  if (server == NULL || owner == NULL) return NULL;
  return owner == &server->owner ? &server->network
                                 : &owner->network_storage;
}

static int chttp_server_owner_cleanup_network(
    chttp_server_impl *server, chttp_server_owner_lane *owner,
    bool retry_timeouts) {
  cnet_client *network;
  int first_status = SALTS_OK;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;

  network = chttp_server_owner_network_storage(server, owner);
  if (owner->network_initialized) {
    int stop_status;
    do {
      stop_status = cnet_client_stop(network, server->config.poll_slice_ms);
    } while (retry_timeouts && stop_status == SALTS_ETIMEDOUT);
    if (stop_status == SALTS_EALREADY) stop_status = SALTS_OK;
    if (first_status == SALTS_OK && stop_status != SALTS_OK)
      first_status = stop_status;
    if (stop_status != SALTS_ETIMEDOUT && stop_status != SALTS_EBUSY) {
      if (owner->manager.impl != NULL) {
        int manager_status = chttp_server_manager_progress(owner);
        if (manager_status == SALTS_OK) manager_status = cnet_manager_destroy(&owner->manager);
        if (manager_status != SALTS_OK)
          return first_status != SALTS_OK ? first_status : manager_status;
      }
      const int destroy_status = cnet_client_destroy(network);
      if (first_status == SALTS_OK && destroy_status != SALTS_OK)
        first_status = destroy_status;
      if (destroy_status == SALTS_OK) {
        owner->network_initialized = false;
        if (owner == &server->owner)
          server->network_initialized = false;
        else
          owner->network = NULL;
      }
    }
  }

  return first_status;
}

static int chttp_server_owner_worker_startup(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  cnet_client_config network_config;
  cnet_client *network;
  int status;
  if (server == NULL || owner == NULL || owner->server != server)
    return SALTS_EINVAL;

  network = chttp_server_owner_network_storage(server, owner);
  if (network == NULL) return SALTS_EINVAL;
  network_config = server->config.network;
  network_config.connection_capacity = owner->connection_count;
  status = cnet_client_init(network, &network_config);
  if (status != SALTS_OK) return status;

  owner->network = network;
  owner->network_initialized = true;
  if (owner == &server->owner) server->network_initialized = true;

  status = cnet_client_set_stream_socket_options(
      network, &server->socket_options.stream);
  if (status != SALTS_OK) return status;

  const cnet_manager_config manager_config = {sizeof(manager_config), CNET_MANAGER_VERSION,
      network, owner->connection_count, owner->connection_count};
  return cnet_manager_init(&owner->manager, &manager_config);
}

static int chttp_server_listener_cleanup(chttp_server_impl *server) {
  int first_status = SALTS_OK;
  int status;
  if (server == NULL) return SALTS_EINVAL;
  if (!server->listener_initialized) return SALTS_OK;

  status = cnet_listener_close(&server->listener);
  if (status != SALTS_OK && status != SALTS_EALREADY)
    first_status = status;

  status = cnet_listener_destroy(&server->listener);
  if (first_status == SALTS_OK && status != SALTS_OK)
    first_status = status;
  if (status == SALTS_OK) server->listener_initialized = false;
  return first_status;
}

static int chttp_server_listener_startup(
    chttp_server_impl *server, uint16_t *out_port) {
  cnet_listener_config listener_config;
  int status;
  if (server == NULL || out_port == NULL) return SALTS_EINVAL;
  *out_port = 0u;
  listener_config = (cnet_listener_config){
      .backend = server->config.network.backend,
      .host = server->host,
      .port = server->config.port,
      .backlog = server->config.backlog};

  status = cnet_listener_init_ex(
      &server->listener, &listener_config, &server->socket_options.listener);
  if (status != SALTS_OK) return status;
  server->listener_initialized = true;
  return cnet_listener_port(&server->listener, out_port);
}

static void chttp_server_request_global_stop(
    chttp_server_impl *server, int terminal_status) {
  cmeta_mutex_lock(&server->mutex);
  if (terminal_status != SALTS_OK &&
      server->stats.terminal_status == SALTS_OK)
    server->stats.terminal_status = terminal_status;
  if (!server->start_called && terminal_status != SALTS_OK) {
    if (server->startup_status == SALTS_OK)
      server->startup_status = terminal_status;
    server->startup_abort = true;
  }
  server->stop_requested = true;
  if (server->start_called && !server->worker_done)
    server->stats.stopping = 1;
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
  chttp_server_wake_owners(server);
}

static void chttp_server_listener_worker(void *user) {
  chttp_server_impl *server = (chttp_server_impl *)user;
  uint16_t port = 0u;
  bool startup_abort = false;
  int status;

  if (server == NULL) return;
  status = chttp_server_listener_startup(server, &port);

  cmeta_mutex_lock(&server->mutex);
  server->listener_terminal_status = status;
  server->listener_startup_reported = true;
  if (status != SALTS_OK) {
    if (server->startup_status == SALTS_OK)
      server->startup_status = status;
    server->startup_abort = true;
  } else {
    server->startup_port = port;
  }
  cmeta_cond_broadcast(&server->changed);
  while (status == SALTS_OK && !server->startup_go &&
         !server->startup_abort)
    cmeta_cond_wait(&server->changed, &server->mutex);
  startup_abort = server->startup_abort;
  cmeta_mutex_unlock(&server->mutex);

  if (status != SALTS_OK || startup_abort) {
    const int cleanup_status = chttp_server_listener_cleanup(server);
    if (status == SALTS_OK && cleanup_status != SALTS_OK)
      status = cleanup_status;
    chttp_server_listener_worker_finish(
        server, status != SALTS_OK ? status : SALTS_ECANCELED);
    return;
  }

  cmeta_mutex_lock(&server->mutex);
  server->listener_ready = true;
  cmeta_cond_broadcast(&server->changed);
  while (!server->start_called && !server->startup_abort)
    cmeta_cond_wait(&server->changed, &server->mutex);
  startup_abort = server->startup_abort;
  cmeta_mutex_unlock(&server->mutex);

  while (status == SALTS_OK && !startup_abort &&
         !chttp_server_should_stop(server)) {
    int ready = 0;
    const uint32_t wait_ms =
        server->config.poll_slice_ms != 0u
            ? server->config.poll_slice_ms
            : 1u;
    status = cnet_listener_wait(&server->listener, wait_ms, &ready);
    if (status != SALTS_OK) break;
    if (ready) status = chttp_server_listener_progress(server);
  }

  if (status != SALTS_OK)
    chttp_server_request_global_stop(server, status);

  {
    const int cleanup_status = chttp_server_listener_cleanup(server);
    if (status == SALTS_OK && cleanup_status != SALTS_OK)
      status = cleanup_status;
  }
  chttp_server_listener_worker_finish(server, status);
}

static void chttp_server_owner_worker(void *user) {
  chttp_server_owner_lane *owner = (chttp_server_owner_lane *)user;
  chttp_server_impl *server;
  cnet_client *network;
  bool startup_abort = false;
  int status;

  if (owner == NULL || owner->server == NULL) return;
  server = owner->server;
  status = chttp_server_owner_worker_startup(server, owner);

  cmeta_mutex_lock(&server->mutex);
  owner->terminal_status = status;
  if (status != SALTS_OK) {
    if (server->startup_status == SALTS_OK)
      server->startup_status = status;
    server->startup_abort = true;
  }
  ++server->startup_reported_count;
  cmeta_cond_broadcast(&server->changed);
  while (status == SALTS_OK && !server->startup_go &&
         !server->startup_abort)
    cmeta_cond_wait(&server->changed, &server->mutex);
  startup_abort = server->startup_abort;
  cmeta_mutex_unlock(&server->mutex);

  if (status != SALTS_OK || startup_abort) {
    (void)chttp_server_owner_cleanup_network(server, owner, true);
    (void)chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
        CHTTP_SERVER_OWNER_RUNTIME_IDLE);
    chttp_server_owner_worker_finish(
        server, owner, status != SALTS_OK ? status : SALTS_ECANCELED);
    return;
  }

  status = chttp_server_owner_runtime_transition(
      owner, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
      CHTTP_SERVER_OWNER_RUNTIME_READY);
  cmeta_mutex_lock(&server->mutex);
  if (status == SALTS_OK)
    ++server->ready_owner_count;
  else {
    if (server->startup_status == SALTS_OK)
      server->startup_status = status;
    server->startup_abort = true;
  }
  cmeta_cond_broadcast(&server->changed);
  cmeta_mutex_unlock(&server->mutex);
  if (status != SALTS_OK) {
    (void)chttp_server_owner_cleanup_network(server, owner, true);
    (void)chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
        CHTTP_SERVER_OWNER_RUNTIME_IDLE);
    chttp_server_request_global_stop(server, status);
    chttp_server_owner_worker_finish(server, owner, status);
    return;
  }

  cmeta_mutex_lock(&server->mutex);
  while (!server->start_called && !server->startup_abort)
    cmeta_cond_wait(&server->changed, &server->mutex);
  startup_abort = server->startup_abort;
  cmeta_mutex_unlock(&server->mutex);
  if (startup_abort) {
    (void)chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_READY,
        CHTTP_SERVER_OWNER_RUNTIME_STOPPING);
  }

  network = chttp_server_owner_network(owner);
  while (status == SALTS_OK && !chttp_server_should_stop(server)) {
    size_t events = 0u;
    status = chttp_server_admission_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_websocket_commands_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server, owner);
    if (status != SALTS_OK) break;

    status = cnet_client_poll(
        network, chttp_server_poll_timeout(server, owner), &events);
    if (status != SALTS_OK) break;

    status = chttp_server_admission_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_websocket_commands_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server, owner);
  }

  if (status != SALTS_OK)
    chttp_server_request_global_stop(server, status);

  /*
   * Listener admission can enqueue and wake this owner from a different
   * thread. Keep every owner CNet client alive until the listener control
   * thread has stopped accepting, closed/destroyed the listener, and published
   * listener_done. This removes the enqueue -> wake versus owner teardown race.
   */
  cmeta_mutex_lock(&server->mutex);
  while (!server->listener_done)
    cmeta_cond_wait(&server->changed, &server->mutex);
  cmeta_mutex_unlock(&server->mutex);

  if (chttp_server_owner_runtime_state_get(owner) ==
      CHTTP_SERVER_OWNER_RUNTIME_READY) {
    const int transition_status = chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_READY,
        CHTTP_SERVER_OWNER_RUNTIME_STOPPING);
    if (status == SALTS_OK && transition_status != SALTS_OK)
      status = transition_status;
  }

  {
    const int shutdown_status = chttp_server_begin_shutdown(server, owner);
    if (status == SALTS_OK && shutdown_status != SALTS_OK)
      status = shutdown_status;
  }
  while (status == SALTS_OK &&
         chttp_server_connections_active(server, owner)) {
    size_t events = 0u;
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server, owner);
    if (status != SALTS_OK) break;
    status = cnet_client_poll(
        network, chttp_server_poll_timeout(server, owner), &events);
    if (status != SALTS_OK) break;
    status = chttp_server_file_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server, owner);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server, owner);
    if (status == SALTS_OK) chttp_server_progress_shutdown(server, owner);
  }

  {
    const int file_status = chttp_server_files_cleanup(server, owner);
    if (status == SALTS_OK && file_status != SALTS_OK) status = file_status;
  }
  {
    const int cleanup_status =
        chttp_server_owner_cleanup_network(server, owner, true);
    if (status == SALTS_OK && cleanup_status != SALTS_OK)
      status = cleanup_status;
  }

  if (chttp_server_owner_runtime_state_get(owner) ==
      CHTTP_SERVER_OWNER_RUNTIME_STOPPING) {
    const int transition_status = chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_STOPPING,
        CHTTP_SERVER_OWNER_RUNTIME_DONE);
    if (status == SALTS_OK && transition_status != SALTS_OK)
      status = transition_status;
  }
  chttp_server_owner_worker_finish(server, owner, status);
}

static cmeta_thread_t *chttp_server_owner_thread_handle(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  if (server == NULL || owner == NULL) return NULL;
  return owner == &server->owner ? &server->thread : &owner->thread;
}

static int chttp_server_join_runtime_threads(chttp_server_impl *server) {
  size_t index;
  int first_status = SALTS_OK;
  if (server == NULL) return SALTS_EINVAL;

  if (server->listener_thread_started) {
    if (cmeta_thread_join(&server->listener_thread) != SALTS_OK) {
      first_status = SALTS_EIO;
    } else {
      cmeta_thread_destroy(&server->listener_thread);
      server->listener_thread_started = false;
    }
  }

  for (index = 0u; index < server->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(server, index);
    cmeta_thread_t *thread;
    if (owner == NULL || !owner->thread_started) continue;
    thread = chttp_server_owner_thread_handle(server, owner);
    if (thread == NULL || cmeta_thread_join(thread) != SALTS_OK) {
      if (first_status == SALTS_OK) first_status = SALTS_EIO;
      continue;
    }
    cmeta_thread_destroy(thread);
    owner->thread_started = false;
    if (owner == &server->owner) server->thread_started = false;
  }
  return first_status;
}

static void chttp_server_reset_failed_start(chttp_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(server, index);
    if (owner == NULL) continue;
    if (chttp_server_owner_runtime_state_get(owner) ==
        CHTTP_SERVER_OWNER_RUNTIME_DONE)
      atomic_store_explicit(
          &owner->runtime_state, CHTTP_SERVER_OWNER_RUNTIME_IDLE,
          memory_order_release);
    owner->terminal_status = SALTS_OK;
  }
  cmeta_mutex_lock(&server->mutex);
  server->started_owner_count = 0u;
  server->startup_reported_count = 0u;
  server->ready_owner_count = 0u;
  server->finished_owner_count = 0u;
  server->startup_port = 0u;
  server->startup_status = SALTS_OK;
  server->listener_terminal_status = SALTS_OK;
  server->listener_startup_reported = false;
  server->listener_ready = false;
  server->listener_done = false;
  server->startup_go = false;
  server->startup_abort = false;
  server->stop_requested = false;
  server->worker_done = false;
  server->stats.port = 0u;
  server->stats.running = 0;
  server->stats.stopping = 0;
  server->stats.terminal_status = SALTS_OK;
  cmeta_mutex_unlock(&server->mutex);
}

int chttp_server_start(chttp_server *server) {
  chttp_server_impl *impl;
  size_t index;
  int status = SALTS_OK;

  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EALREADY;

  cmeta_mutex_lock(&impl->mutex);
  impl->started_owner_count = impl->owner_count;
  impl->startup_reported_count = 0u;
  impl->ready_owner_count = 0u;
  impl->finished_owner_count = 0u;
  impl->startup_port = 0u;
  impl->startup_status = SALTS_OK;
  impl->listener_terminal_status = SALTS_OK;
  impl->listener_startup_reported = false;
  impl->listener_ready = false;
  impl->listener_done = false;
  impl->startup_go = false;
  impl->startup_abort = false;
  impl->stop_requested = false;
  impl->worker_done = false;
  impl->stats.port = 0u;
  impl->stats.running = 0;
  impl->stats.stopping = 0;
  impl->stats.terminal_status = SALTS_OK;
  cmeta_mutex_unlock(&impl->mutex);

  for (index = 0u; index < impl->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    status = chttp_server_owner_runtime_transition(
        owner, CHTTP_SERVER_OWNER_RUNTIME_IDLE,
        CHTTP_SERVER_OWNER_RUNTIME_STARTING);
    if (status != SALTS_OK) {
      size_t rollback;
      for (rollback = 0u; rollback < index; ++rollback) {
        chttp_server_owner_lane *previous =
            chttp_server_owner_at(impl, rollback);
        (void)chttp_server_owner_runtime_transition(
            previous, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
            CHTTP_SERVER_OWNER_RUNTIME_IDLE);
      }
      chttp_server_reset_failed_start(impl);
      return status;
    }
  }

  status = cmeta_thread_create(
      &impl->listener_thread, chttp_server_listener_worker, impl);
  if (status != SALTS_OK) {
    for (index = 0u; index < impl->owner_count; ++index) {
      chttp_server_owner_lane *pending =
          chttp_server_owner_at(impl, index);
      if (pending != NULL &&
          chttp_server_owner_runtime_state_get(pending) ==
              CHTTP_SERVER_OWNER_RUNTIME_STARTING)
        (void)chttp_server_owner_runtime_transition(
            pending, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
            CHTTP_SERVER_OWNER_RUNTIME_IDLE);
    }
    chttp_server_reset_failed_start(impl);
    return SALTS_EIO;
  }
  impl->listener_thread_started = true;

  for (index = 0u; index < impl->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    cmeta_thread_t *thread = chttp_server_owner_thread_handle(impl, owner);
    status = cmeta_thread_create(
        thread, chttp_server_owner_worker, owner);
    if (status != SALTS_OK) {
      size_t uncreated;
      cmeta_mutex_lock(&impl->mutex);
      impl->startup_status = SALTS_EIO;
      impl->startup_abort = true;
      cmeta_cond_broadcast(&impl->changed);
      cmeta_mutex_unlock(&impl->mutex);
      for (uncreated = index; uncreated < impl->owner_count; ++uncreated) {
        chttp_server_owner_lane *pending =
            chttp_server_owner_at(impl, uncreated);
        if (pending != NULL &&
            chttp_server_owner_runtime_state_get(pending) ==
                CHTTP_SERVER_OWNER_RUNTIME_STARTING)
          (void)chttp_server_owner_runtime_transition(
              pending, CHTTP_SERVER_OWNER_RUNTIME_STARTING,
              CHTTP_SERVER_OWNER_RUNTIME_IDLE);
      }
      (void)chttp_server_join_runtime_threads(impl);
      chttp_server_reset_failed_start(impl);
      return SALTS_EIO;
    }
    owner->thread_started = true;
    if (owner == &impl->owner) impl->thread_started = true;
  }

  cmeta_mutex_lock(&impl->mutex);
  while ((impl->startup_reported_count < impl->owner_count ||
          !impl->listener_startup_reported) &&
         !impl->startup_abort)
    cmeta_cond_wait(&impl->changed, &impl->mutex);
  if (impl->startup_abort) {
    status = impl->startup_status == SALTS_OK
                 ? SALTS_EIO
                 : impl->startup_status;
    cmeta_cond_broadcast(&impl->changed);
    cmeta_mutex_unlock(&impl->mutex);
    (void)chttp_server_join_runtime_threads(impl);
    chttp_server_reset_failed_start(impl);
    return status;
  }

  impl->stats.port = impl->startup_port;
  impl->stats.running = 1;
  impl->stats.stopping = 0;
  impl->stats.terminal_status = SALTS_OK;
  impl->startup_go = true;
  cmeta_cond_broadcast(&impl->changed);
  while ((impl->ready_owner_count < impl->owner_count ||
          !impl->listener_ready) &&
         !impl->startup_abort)
    cmeta_cond_wait(&impl->changed, &impl->mutex);
  if (impl->startup_abort) {
    status = impl->startup_status == SALTS_OK
                 ? SALTS_EIO
                 : impl->startup_status;
    impl->stop_requested = true;
    impl->stats.stopping = 1;
    cmeta_cond_broadcast(&impl->changed);
    cmeta_mutex_unlock(&impl->mutex);
    chttp_server_wake_owners(impl);
    (void)chttp_server_join_runtime_threads(impl);
    chttp_server_reset_failed_start(impl);
    return status;
  }

  impl->start_called = true;
  cmeta_cond_broadcast(&impl->changed);
  cmeta_mutex_unlock(&impl->mutex);
  return SALTS_OK;
}

int chttp_server_port(const chttp_server *server, uint16_t *out_port) {
  const chttp_server_impl *impl;
  if (out_port == NULL) return SALTS_EINVAL;
  *out_port = 0u;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (const chttp_server_impl *)server->impl;
  cmeta_mutex_lock((cmeta_mutex_t *)&impl->mutex);
  if (!impl->start_called) {
    cmeta_mutex_unlock((cmeta_mutex_t *)&impl->mutex);
    return SALTS_EINVAL;
  }
  *out_port = impl->stats.port;
  cmeta_mutex_unlock((cmeta_mutex_t *)&impl->mutex);
  return SALTS_OK;
}

int chttp_server_stop(chttp_server *server, uint32_t timeout_ms) {
  chttp_server_impl *impl;
  const uint64_t started_ms = cmeta_monotonic_ms();
  int terminal_status;
  int join_status;

  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (chttp_active_callback_server == impl) return SALTS_EBUSY;

  cmeta_mutex_lock(&impl->mutex);
  if (!impl->start_called) {
    cmeta_mutex_unlock(&impl->mutex);
    return SALTS_OK;
  }
  impl->stop_requested = true;
  if (!impl->worker_done) impl->stats.stopping = 1;
  cmeta_cond_broadcast(&impl->changed);
  cmeta_mutex_unlock(&impl->mutex);
  chttp_server_wake_owners(impl);

  cmeta_mutex_lock(&impl->mutex);
  while (!impl->worker_done) {
    if (timeout_ms == 0u) {
      cmeta_cond_wait(&impl->changed, &impl->mutex);
    } else {
      const uint64_t elapsed_ms = cmeta_monotonic_ms() - started_ms;
      uint64_t remaining_ns;
      if (elapsed_ms >= timeout_ms) {
        cmeta_mutex_unlock(&impl->mutex);
        return SALTS_ETIMEDOUT;
      }
      remaining_ns = ((uint64_t)timeout_ms - elapsed_ms) * 1000000u;
      if (cmeta_cond_timedwait(
              &impl->changed, &impl->mutex, remaining_ns) != SALTS_OK &&
          !impl->worker_done) {
        cmeta_mutex_unlock(&impl->mutex);
        return SALTS_ETIMEDOUT;
      }
    }
  }
  terminal_status = impl->stats.terminal_status;
  cmeta_mutex_unlock(&impl->mutex);

  join_status = chttp_server_join_runtime_threads(impl);
  if (terminal_status == SALTS_OK && join_status != SALTS_OK)
    terminal_status = join_status;
  return terminal_status;
}

int chttp_server_destroy(chttp_server *server) {
  chttp_server_impl *impl;
  size_t index;
  int join_status;

  if (server == NULL) return SALTS_EINVAL;
  if (server->impl == NULL) return SALTS_OK;
  impl = (chttp_server_impl *)server->impl;

  cmeta_mutex_lock(&impl->mutex);
  if (impl->stats.running || impl->stats.stopping ||
      (impl->start_called && !impl->worker_done)) {
    cmeta_mutex_unlock(&impl->mutex);
    return SALTS_EBUSY;
  }
  cmeta_mutex_unlock(&impl->mutex);

  join_status = chttp_server_join_runtime_threads(impl);
  if (join_status != SALTS_OK) return join_status;

  for (index = 0u; index < impl->owner_count; ++index) {
    chttp_server_owner_lane *owner = chttp_server_owner_at(impl, index);
    if (owner == NULL) return SALTS_EPROTO;
    if (owner->file_runtime_initialized) {
      const int cleanup_status = chttp_server_files_cleanup(impl, owner);
      if (cleanup_status != SALTS_OK) return cleanup_status;
    }
    if (owner->network_initialized ||
        chttp_server_owner_lease_count(owner) != 0u)
      return SALTS_EBUSY;
  }
  if (impl->network_initialized || impl->listener_initialized)
    return SALTS_EBUSY;

  chttp_server_impl_free(impl);
  server->impl = NULL;
  return SALTS_OK;
}

int chttp_server_get_stats(const chttp_server *server, chttp_server_stats *out_stats) {
  const chttp_server_impl *impl;
  if (out_stats == NULL) return SALTS_EINVAL;
  *out_stats = (chttp_server_stats){0};
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (const chttp_server_impl *)server->impl;
  cmeta_mutex_lock((cmeta_mutex_t *)&impl->mutex);
  *out_stats = impl->stats;
  cmeta_mutex_unlock((cmeta_mutex_t *)&impl->mutex);
  out_stats->buffer_bytes = atomic_load_explicit(&impl->buffer_bytes, memory_order_acquire);
  out_stats->peak_buffer_bytes =
      atomic_load_explicit(&impl->peak_buffer_bytes, memory_order_acquire);
  out_stats->rejected_buffer_allocations =
      atomic_load_explicit(&impl->rejected_buffer_allocations, memory_order_acquire);
  return SALTS_OK;
}
