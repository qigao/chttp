#include "chttp_h2_server.h"
#include "chttp_jwt_internal.h"
#include "chttp_server_runtime.h"
#include "chttp_tls.h"

#include <salts/clock.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static cflow_io_native_backend_kind chttp_server_file_backend(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_IO_URING;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static void chttp_server_file_wake(void *user) {
  chttp_server_impl *server = (chttp_server_impl *)user;
  if (server != NULL && server->network_initialized) (void)cnet_client_wake(&server->network);
}

int chttp_server_file_runtime_ensure(chttp_server_impl *server,
                                     cflow_io_file_runtime **out_runtime) {
  cflow_io_file_runtime_config config;
  size_t command_capacity;
  int status;
  if (server == NULL || out_runtime == NULL || !server->network_initialized ||
      server->file_transfer_capacity == 0u)
    return SALTS_EINVAL;
  *out_runtime = NULL;
  if (!server->file_runtime_initialized) {
    command_capacity = server->file_transfer_capacity <= SIZE_MAX / 2u
                           ? server->file_transfer_capacity * 2u
                           : server->file_transfer_capacity;
    config =
        (cflow_io_file_runtime_config){.backend_kind = chttp_server_file_backend(),
                                       .file_capacity = server->file_transfer_capacity,
                                       .request_capacity = server->file_transfer_capacity,
                                       .command_capacity = command_capacity,
                                       .completion_batch_capacity = server->file_transfer_capacity,
                                       .wake = chttp_server_file_wake,
                                       .wake_user = server};
    status = cflow_io_file_runtime_init(&server->file_runtime, &config);
    if (status != SALTS_OK) return status;
    server->file_runtime_initialized = true;
  }
  *out_runtime = &server->file_runtime;
  return SALTS_OK;
}

int chttp_server_file_transfer_register(chttp_server_impl *server, chttp_file_transfer *transfer) {
  size_t index;
  if (server == NULL || transfer == NULL || transfer->file.impl == NULL) return SALTS_EINVAL;
  for (index = 0u; index < server->file_transfer_capacity; ++index) {
    if (server->file_transfers[index] == NULL) {
      server->file_transfers[index] = transfer;
      return SALTS_OK;
    }
  }
  return SALTS_ENOBUFS;
}

static int chttp_server_file_progress(chttp_server_impl *server) {
  size_t progressed = 0u;
  size_t max_steps;
  size_t index;
  int status;
  if (server == NULL || !server->file_runtime_initialized) return SALTS_OK;
  max_steps = server->file_transfer_capacity <= SIZE_MAX / 4u ? server->file_transfer_capacity * 4u
                                                              : SIZE_MAX;
  status = cflow_io_file_runtime_run_ready(&server->file_runtime, max_steps, &progressed);
  if (status != SALTS_OK) return status;
  for (index = 0u; index < server->file_transfer_capacity; ++index) {
    chttp_file_transfer *transfer = server->file_transfers[index];
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
    server->file_transfers[index] = NULL;
  }
  return SALTS_OK;
}

static bool chttp_server_files_active(const chttp_server_impl *server) {
  size_t index;
  if (server == NULL) return false;
  for (index = 0u; index < server->file_transfer_capacity; ++index)
    if (server->file_transfers[index] != NULL) return true;
  return false;
}

static int chttp_server_files_cleanup(chttp_server_impl *server) {
  size_t index;
  int status;
  if (server == NULL || !server->file_runtime_initialized) return SALTS_OK;
  for (index = 0u; index < server->config.network.connection_capacity; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    chttp_server_response_builder *builder = &connection->request_state.response_builder;
    if (builder->file_transfer != NULL)
      chttp_server_response_builder_close_source(builder, SALTS_ECANCELED);
    chttp_h2_server_connection_cancel_file_sources(connection->h2);
  }
  for (index = 0u; index < server->file_transfer_capacity; ++index) {
    chttp_file_transfer *transfer = server->file_transfers[index];
    if (transfer == NULL) continue;
    chttp_file_transfer_set_ready(transfer, NULL, NULL);
    transfer->owner_release_requested = true;
    status = chttp_file_transfer_close(transfer);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
  }
  while (chttp_server_files_active(server)) {
    status = chttp_server_file_progress(server);
    if (status != SALTS_OK) return status;
    for (index = 0u; index < server->file_transfer_capacity; ++index) {
      chttp_file_transfer *transfer = server->file_transfers[index];
      if (transfer == NULL || transfer->close_requested) continue;
      transfer->owner_release_requested = true;
      status = chttp_file_transfer_close(transfer);
      if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
    }
    salts_thread_yield();
  }
  status = cflow_io_file_runtime_close(&server->file_runtime);
  if (status != SALTS_OK && status != SALTS_EALREADY) return status;
  while (!cflow_io_file_runtime_is_quiescent(&server->file_runtime)) {
    status = chttp_server_file_progress(server);
    if (status != SALTS_OK) return status;
    salts_thread_yield();
  }
  status = cflow_io_file_runtime_destroy(&server->file_runtime);
  if (status == SALTS_OK) server->file_runtime_initialized = false;
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
  if (connection == NULL || connection->server == NULL) return SALTS_EINVAL;
  return chttp_server_buffer_grow(
      connection->server, &connection->outbound, &connection->outbound_capacity, required,
      connection->server->config.network.max_send_bytes, connection->outbound_size);
}

void chttp_server_connection_release_outbound(chttp_server_connection *connection) {
  if (connection == NULL || connection->outbound == NULL) return;
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

static uint32_t chttp_server_poll_timeout(const chttp_server_impl *server) {
  enum { CHTTP_SERVER_FILE_PROGRESS_POLL_MS = 1u };
  if (server != NULL && chttp_server_files_active(server) &&
      server->config.poll_slice_ms > CHTTP_SERVER_FILE_PROGRESS_POLL_MS)
    return CHTTP_SERVER_FILE_PROGRESS_POLL_MS;
  return server == NULL ? 0u : server->config.poll_slice_ms;
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
  if (validator == NULL) return SALTS_OK;

  status = chttp_jwt_bearer_request_validate(state, request, validator);
  if (status != SALTS_OK) state->admission_rejected = true;
  return status;
}

void chttp_server_request_state_reset(chttp_server_request_state *state) {
  if (state == NULL) return;
  chttp_server_request_body_close(state, SALTS_ECANCELED);
  chttp_server_request_admission_clear(state);
  chttp_server_response_builder_reset(&state->response_builder);
  state->param_storage_used = 0u;
  state->param_count = 0u;
  state->session.impl = NULL;
  state->session_context = (chttp_session_context){0};
  state->body_route = NULL;
  state->body_sink = (chttp_body_sink){0};
  state->body_was_streamed = false;
}

void chttp_server_request_state_destroy(chttp_server_request_state *state) {
  if (state == NULL) return;
  chttp_server_request_body_close(state, SALTS_ECANCELED);
  chttp_jwt_request_state_reset(state);
  chttp_server_response_builder_destroy(&state->response_builder);
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

static void chttp_server_impl_free(chttp_server_impl *impl) {
  size_t index;
  if (impl == NULL) return;
  if (impl->websocket_commands != NULL)
    for (index = 0u; index < impl->config.network.command_capacity; ++index)
      free(impl->websocket_commands[index].data);
  if (impl->connections != NULL)
    for (index = 0u; index < impl->config.network.connection_capacity; ++index)
      chttp_server_connection_destroy(&impl->connections[index]);
  chttp_session_store_destroy(impl);
  free(impl->file_transfers);
  free(impl->websocket_commands);
  free(impl->connections);
  free(impl->middleware);
  free(impl->route_middleware);
  free(impl->route_paths);
  free(impl->routes);
  free(impl->session_cookie_name);
  free(impl->host);
  if (impl->tls_initialized) (void)cnet_tls_server_destroy(&impl->tls_server);
  if (impl->sync_initialized) {
    salts_cond_destroy(&impl->changed);
    salts_mutex_destroy(&impl->mutex);
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
  impl->socket_options = (chttp_server_socket_options)CHTTP_SERVER_SOCKET_OPTIONS_INIT;
  impl->file_transfer_capacity = file_transfer_capacity;
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
  impl->file_transfers =
      (chttp_file_transfer **)calloc(file_transfer_capacity, sizeof(*impl->file_transfers));
  impl->websocket_commands = (chttp_server_websocket_command *)calloc(
      config->network.command_capacity, sizeof(*impl->websocket_commands));
  if (impl->host == NULL || impl->session_cookie_name == NULL || impl->routes == NULL ||
      impl->route_paths == NULL ||
      (route_middleware_count != 0u && impl->route_middleware == NULL) ||
      (config->middleware_capacity != 0u && impl->middleware == NULL) ||
      impl->connections == NULL || impl->file_transfers == NULL ||
      impl->websocket_commands == NULL) {
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
    status = chttp_server_connection_init(impl, &impl->connections[index]);
    if (status != SALTS_OK) {
      chttp_server_impl_free(impl);
      return status;
    }
  }
  salts_mutex_init(&impl->mutex);
  salts_cond_init(&impl->changed);
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

static void chttp_server_stats_update(chttp_server_impl *server, int field) {
  salts_mutex_lock(&server->mutex);
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
  salts_mutex_unlock(&server->mutex);
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
    status = cnet_receive(&connection->server->network, connection->handle, 1u);
  else if (action == CHTTP_SERVER_PENDING_SEND)
    status = connection->close_after_write
                 ? cnet_send_and_close(&connection->server->network, connection->handle,
                                       connection->outbound, connection->outbound_size)
                 : cnet_send(&connection->server->network, connection->handle, connection->outbound,
                             connection->outbound_size);
  else status = cnet_close(&connection->server->network, connection->handle);
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
  status = cnet_tls_negotiated_alpn(&connection->server->network, connection->handle, alpn,
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
      status = cnet_tls_peer_certificate_sha256(&connection->server->network, connection->handle,
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
        connection->h2_close_after_ms = chttp_server_deadline_after(salts_monotonic_ms(), quiet_ms);
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
        deferred_state == CHTTP_SERVER_DEFERRED_IDLE)
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
  status = chttp_server_protocol_input(connection, view->data, view->size, &http_status);
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

static int chttp_server_deferred_input_resume(chttp_server_connection *connection) {
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
  int status = SALTS_OK;
  if (!chttp_server_connection_matches(connection, handle) || !connection->writing ||
      size != connection->outbound_size) {
    chttp_server_connection_close(connection);
    return;
  }
  connection->writing = false;
  connection->outbound_size = 0u;
  connection->deferred_response_writing = false;
  if (connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_NONE)
    status = chttp_server_websocket_send_complete(connection);
  if (connection->response_streaming) status = chttp_server_response_stream_next(connection);
  if (connection->outbound_size == 0u && !connection->response_streaming &&
      connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE &&
      connection->wire_protocol != CHTTP_SERVER_WIRE_HTTP_2)
    chttp_server_connection_release_outbound(connection);
  if (status == SALTS_OK && resume_deferred && !connection->close_after_write)
    status = chttp_server_deferred_input_resume(connection);
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
  status = chttp_jwt_bearer_unauthorized_response(&state->response);
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
  if (state->body_sink_open) return SALTS_EBUSY;
  if (!state->admission_complete || state->admission_rejected) return SALTS_EPERM;

  state->body_route = NULL;
  state->body_sink = (chttp_body_sink){0};
  state->body_was_streamed = false;
  route = state->admitted_route;
  if (route == NULL || route->body_open == NULL) return SALTS_OK;

  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = NULL;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
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
  state->body_sink_open = true;
  state->body_was_streamed = true;
  return SALTS_OK;
}

int chttp_server_request_body_write(chttp_server_request_state *state, const void *data,
                                    size_t size) {
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

  server = state->server;
  route = state->admitted_route;
  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = server->config.session_capacity == 0u ? NULL : &state->session;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
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
    status =
        chttp_server_response_serialize(builder, request, connection->outbound,
                                        connection->outbound_capacity, &connection->outbound_size);
    if (status != SALTS_OK) chttp_session_request_abort(&connection->request_state);
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
    }
  }
  return status;
}

static chttp_server_connection *chttp_server_free_connection(chttp_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->config.network.connection_capacity; ++index)
    if (!server->connections[index].active &&
        chttp_server_deferred_token_state(atomic_load_explicit(
            &server->connections[index].deferred_token, memory_order_acquire)) ==
            CHTTP_SERVER_DEFERRED_IDLE &&
        !chttp_h2_server_connection_has_deferred(server->connections[index].h2))
      return &server->connections[index];
  return NULL;
}

static int chttp_server_accept_ready(chttp_server_impl *server) {
  for (;;) {
    chttp_server_connection *connection = chttp_server_free_connection(server);
    cnet_observer observer;
    cnet_connection handle = {0};
    cnet_stream_peer peer = {0};
    int status;
    if (connection == NULL) return SALTS_OK;
    observer = (cnet_observer){.on_state = chttp_server_on_state,
                               .on_receive = chttp_server_on_receive,
                               .on_send = chttp_server_on_send,
                               .user = connection};
    status = server->tls_initialized
                 ? cnet_listener_accept_tls_peer(&server->listener, &server->network,
                                                 &server->tls_server, &observer, &handle, &peer)
                 : cnet_listener_accept_peer(&server->listener, &server->network, &observer,
                                             &handle, &peer);
    if (status == SALTS_ETIMEDOUT) return SALTS_OK;
    if (status == SALTS_ENOBUFS) {
      salts_mutex_lock(&server->mutex);
      ++server->stats.rejected_connections;
      salts_mutex_unlock(&server->mutex);
      return SALTS_OK;
    }
    if (status != SALTS_OK) return status;
    if (server->config.enable_http2) {
      status = chttp_h2_server_connection_prepare(connection->h2);
      if (status != SALTS_OK) {
        (void)cnet_close(&server->network, handle);
        return status;
      }
    }
    connection->handle = handle;
    connection->peer = peer;
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
    connection->pending_action = CHTTP_SERVER_PENDING_NONE;
    connection->outbound_size = 0u;
    connection->h2_close_after_ms = 0u;
    connection->protocol_prefix_size = 0u;
    connection->wire_protocol =
        server->config.enable_http2 ? CHTTP_SERVER_WIRE_UNKNOWN : CHTTP_SERVER_WIRE_HTTP_1_1;
    chttp_server_websocket_reset(connection);
    chttp_server_request_state_reset(&connection->request_state);
    status = chttp_server_parser_reset(&connection->parser);
    if (status != SALTS_OK) return status;
    chttp_server_stats_connection_open(server);
  }
}

static int chttp_server_deferred_progress(chttp_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->config.network.connection_capacity; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    chttp_server_response_builder *builder = &connection->deferred_builder;
    const uint_fast64_t deferred_token =
        atomic_load_explicit(&connection->deferred_token, memory_order_acquire);
    const chttp_server_deferred_state deferred_state =
        chttp_server_deferred_token_state(deferred_token);
    int status;
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

static int chttp_server_retry_pending(chttp_server_impl *server) {
  const size_t capacity = server->config.network.connection_capacity;
  size_t offset;
  for (offset = 0u; offset < capacity; ++offset) {
    const size_t index = (server->pending_retry_cursor + offset) % capacity;
    chttp_server_connection *connection = &server->connections[index];
    int status;
    if (!connection->active || connection->pending_action == CHTTP_SERVER_PENDING_NONE) continue;
    status = chttp_server_connection_retry(connection);
    server->pending_retry_cursor = (index + 1u) % capacity;
    if (status == SALTS_ENOBUFS) return SALTS_OK;
    if (status != SALTS_OK && status != SALTS_EBUSY) return status;
  }
  return SALTS_OK;
}

static bool chttp_server_should_stop(chttp_server_impl *server) {
  bool stop;
  salts_mutex_lock(&server->mutex);
  stop = server->stop_requested;
  salts_mutex_unlock(&server->mutex);
  return stop;
}

static bool chttp_server_connections_active(const chttp_server_impl *server) {
  size_t index;
  for (index = 0u; index < server->config.network.connection_capacity; ++index)
    if (server->connections[index].active ||
        chttp_server_deferred_token_state(atomic_load_explicit(
            &server->connections[index].deferred_token, memory_order_acquire)) !=
            CHTTP_SERVER_DEFERRED_IDLE ||
        chttp_h2_server_connection_has_deferred(server->connections[index].h2))
      return true;
  return false;
}

static int chttp_server_begin_shutdown(chttp_server_impl *server) {
  size_t index;
  int status;
  if (server->listener_initialized) {
    status = cnet_listener_close(&server->listener);
    if (status != SALTS_OK && status != SALTS_EALREADY) return status;
  }
  for (index = 0u; index < server->config.network.connection_capacity; ++index) {
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

static void chttp_server_progress_shutdown(chttp_server_impl *server) {
  const uint64_t now_ms = salts_monotonic_ms();
  size_t index;
  for (index = 0u; index < server->config.network.connection_capacity; ++index) {
    chttp_server_connection *connection = &server->connections[index];
    if (!connection->active || connection->wire_protocol != CHTTP_SERVER_WIRE_HTTP_2 ||
        connection->h2_close_after_ms == 0u || now_ms < connection->h2_close_after_ms ||
        !chttp_h2_server_connection_stop_waiting(connection->h2))
      continue;
    connection->h2_close_after_ms = 0u;
    chttp_server_connection_close(connection);
  }
}

static void chttp_server_worker_finish(chttp_server_impl *server, int terminal_status) {
  salts_mutex_lock(&server->mutex);
  server->stats.running = 0;
  server->stats.stopping = 0;
  server->stats.terminal_status = terminal_status;
  server->worker_done = true;
  salts_cond_broadcast(&server->changed);
  salts_mutex_unlock(&server->mutex);
}

static int chttp_server_cleanup_network(chttp_server_impl *server, bool retry_timeouts) {
  int first_status = SALTS_OK;
  if (server->listener_initialized) {
    const int close_status = cnet_listener_close(&server->listener);
    if (close_status != SALTS_OK && close_status != SALTS_EALREADY) first_status = close_status;
  }
  if (server->network_initialized) {
    int stop_status;
    do {
      stop_status = cnet_client_stop(&server->network, server->config.poll_slice_ms);
    } while (retry_timeouts && stop_status == SALTS_ETIMEDOUT);
    if (stop_status == SALTS_EALREADY) stop_status = SALTS_OK;
    if (first_status == SALTS_OK && stop_status != SALTS_OK) first_status = stop_status;
    if (stop_status != SALTS_ETIMEDOUT && stop_status != SALTS_EBUSY) {
      const int destroy_status = cnet_client_destroy(&server->network);
      if (first_status == SALTS_OK && destroy_status != SALTS_OK) first_status = destroy_status;
      if (destroy_status == SALTS_OK) server->network_initialized = false;
    }
  }
  if (server->listener_initialized) {
    const int destroy_status = cnet_listener_destroy(&server->listener);
    if (first_status == SALTS_OK && destroy_status != SALTS_OK) first_status = destroy_status;
    if (destroy_status == SALTS_OK) server->listener_initialized = false;
  }
  return first_status;
}

static void chttp_server_worker(void *user) {
  chttp_server_impl *server = (chttp_server_impl *)user;
  int status = SALTS_OK;
  while (!chttp_server_should_stop(server)) {
    int ready = 0;
    size_t events = 0u;
    status = chttp_server_file_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_websocket_commands_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server);
    if (status != SALTS_OK) break;
    status = cnet_listener_wait(&server->listener, 0u, &ready);
    if (status != SALTS_OK) break;
    if (ready) {
      status = chttp_server_accept_ready(server);
      if (status != SALTS_OK) break;
    }
    status = cnet_client_poll(&server->network, chttp_server_poll_timeout(server), &events);
    if (status != SALTS_OK) break;
    status = chttp_server_file_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_deferred_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_websocket_commands_progress(server);
    if (status != SALTS_OK) break;
    status = chttp_server_retry_pending(server);
    if (status != SALTS_OK) break;
  }
  if (status == SALTS_OK) {
    status = chttp_server_begin_shutdown(server);
    while (status == SALTS_OK && chttp_server_connections_active(server)) {
      size_t events = 0u;
      status = chttp_server_file_progress(server);
      if (status != SALTS_OK) break;
      status = chttp_server_deferred_progress(server);
      if (status != SALTS_OK) break;
      status = chttp_server_retry_pending(server);
      if (status != SALTS_OK) break;
      status = cnet_client_poll(&server->network, chttp_server_poll_timeout(server), &events);
      if (status != SALTS_OK) break;
      status = chttp_server_file_progress(server);
      if (status != SALTS_OK) break;
      status = chttp_server_deferred_progress(server);
      if (status != SALTS_OK) break;
      status = chttp_server_retry_pending(server);
      if (status == SALTS_OK) chttp_server_progress_shutdown(server);
    }
  }
  {
    const int file_status = chttp_server_files_cleanup(server);
    if (status == SALTS_OK && file_status != SALTS_OK) status = file_status;
  }
  {
    const int cleanup_status = chttp_server_cleanup_network(server, true);
    if (status == SALTS_OK && cleanup_status != SALTS_OK) status = cleanup_status;
  }
  chttp_server_worker_finish(server, status);
}

int chttp_server_start(chttp_server *server) {
  chttp_server_impl *impl;
  cnet_listener_config listener_config;
  uint16_t port = 0u;
  int status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called || impl->thread_started) return SALTS_EALREADY;
  status = cnet_client_init(&impl->network, &impl->config.network);
  if (status != SALTS_OK) return status;
  impl->network_initialized = true;
  status = cnet_client_set_stream_socket_options(&impl->network, &impl->socket_options.stream);
  if (status != SALTS_OK) {
    (void)chttp_server_cleanup_network(impl, false);
    return status;
  }
  listener_config = (cnet_listener_config){.backend = impl->config.network.backend,
                                           .host = impl->host,
                                           .port = impl->config.port,
                                           .backlog = impl->config.backlog};
  status = cnet_listener_init_ex(&impl->listener, &listener_config, &impl->socket_options.listener);
  if (status == SALTS_OK) {
    impl->listener_initialized = true;
    status = cnet_listener_port(&impl->listener, &port);
  }
  if (status != SALTS_OK) {
    (void)chttp_server_cleanup_network(impl, false);
    return status;
  }
  salts_mutex_lock(&impl->mutex);
  impl->stats.port = port;
  impl->stats.running = 1;
  impl->stats.stopping = 0;
  impl->stats.terminal_status = SALTS_OK;
  impl->stop_requested = false;
  impl->worker_done = false;
  salts_mutex_unlock(&impl->mutex);
  status = salts_thread_create(&impl->thread, chttp_server_worker, impl);
  if (status != SALTS_OK) {
    (void)chttp_server_cleanup_network(impl, false);
    salts_mutex_lock(&impl->mutex);
    impl->stats.running = 0;
    impl->stats.terminal_status = SALTS_EIO;
    salts_mutex_unlock(&impl->mutex);
    return SALTS_EIO;
  }
  impl->thread_started = true;
  impl->start_called = true;
  return SALTS_OK;
}

int chttp_server_port(const chttp_server *server, uint16_t *out_port) {
  const chttp_server_impl *impl;
  if (out_port == NULL) return SALTS_EINVAL;
  *out_port = 0u;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (const chttp_server_impl *)server->impl;
  salts_mutex_lock((salts_mutex_t *)&impl->mutex);
  if (!impl->start_called) {
    salts_mutex_unlock((salts_mutex_t *)&impl->mutex);
    return SALTS_EINVAL;
  }
  *out_port = impl->stats.port;
  salts_mutex_unlock((salts_mutex_t *)&impl->mutex);
  return SALTS_OK;
}

int chttp_server_stop(chttp_server *server, uint32_t timeout_ms) {
  chttp_server_impl *impl;
  const uint64_t started_ms = salts_monotonic_ms();
  int terminal_status;
  if (server == NULL || server->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (chttp_active_callback_server == impl) return SALTS_EBUSY;
  salts_mutex_lock(&impl->mutex);
  if (!impl->start_called) {
    salts_mutex_unlock(&impl->mutex);
    return SALTS_OK;
  }
  impl->stop_requested = true;
  if (!impl->worker_done) impl->stats.stopping = 1;
  while (!impl->worker_done) {
    if (timeout_ms == 0u) salts_cond_wait(&impl->changed, &impl->mutex);
    else {
      const uint64_t elapsed_ms = salts_monotonic_ms() - started_ms;
      uint64_t remaining_ns;
      if (elapsed_ms >= timeout_ms) {
        salts_mutex_unlock(&impl->mutex);
        return SALTS_ETIMEDOUT;
      }
      remaining_ns = ((uint64_t)timeout_ms - elapsed_ms) * 1000000u;
      if (salts_cond_timedwait(&impl->changed, &impl->mutex, remaining_ns) != SALTS_OK &&
          !impl->worker_done) {
        salts_mutex_unlock(&impl->mutex);
        return SALTS_ETIMEDOUT;
      }
    }
  }
  terminal_status = impl->stats.terminal_status;
  salts_mutex_unlock(&impl->mutex);
  if (impl->thread_started) {
    if (salts_thread_join(&impl->thread) != SALTS_OK) return SALTS_EIO;
    salts_thread_destroy(&impl->thread);
    impl->thread_started = false;
  }
  {
    const int cleanup_status = chttp_server_cleanup_network(impl, false);
    if (terminal_status == SALTS_OK && cleanup_status != SALTS_OK) terminal_status = cleanup_status;
  }
  return terminal_status;
}

int chttp_server_destroy(chttp_server *server) {
  chttp_server_impl *impl;
  if (server == NULL) return SALTS_EINVAL;
  if (server->impl == NULL) return SALTS_OK;
  impl = (chttp_server_impl *)server->impl;
  salts_mutex_lock(&impl->mutex);
  if (impl->stats.running || impl->stats.stopping || (impl->start_called && !impl->worker_done)) {
    salts_mutex_unlock(&impl->mutex);
    return SALTS_EBUSY;
  }
  salts_mutex_unlock(&impl->mutex);
  if (impl->thread_started) {
    if (salts_thread_join(&impl->thread) != SALTS_OK) return SALTS_EIO;
    salts_thread_destroy(&impl->thread);
    impl->thread_started = false;
  }
  {
    const int cleanup_status = chttp_server_cleanup_network(impl, false);
    if (cleanup_status != SALTS_OK) return cleanup_status;
  }
  {
    const int cleanup_status = chttp_server_files_cleanup(impl);
    if (cleanup_status != SALTS_OK) return cleanup_status;
  }
  if (impl->network_initialized || impl->listener_initialized) return SALTS_EBUSY;
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
  salts_mutex_lock((salts_mutex_t *)&impl->mutex);
  *out_stats = impl->stats;
  salts_mutex_unlock((salts_mutex_t *)&impl->mutex);
  out_stats->buffer_bytes = atomic_load_explicit(&impl->buffer_bytes, memory_order_acquire);
  out_stats->peak_buffer_bytes =
      atomic_load_explicit(&impl->peak_buffer_bytes, memory_order_acquire);
  out_stats->rejected_buffer_allocations =
      atomic_load_explicit(&impl->rejected_buffer_allocations, memory_order_acquire);
  return SALTS_OK;
}
