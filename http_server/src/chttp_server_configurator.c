#include "chttp_server_runtime.h"
#include "chttp_h2_server.h"
#include "chttp_tls.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Startup configuration is prepared in private storage, then published once.
 * No runtime worker may observe partially configured topology or bindings. */
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

static void chttp_server_owner_storage_release(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t index;
  if (server == NULL || owner == NULL) return;
  /* Storage is released before startup or after joining all runtime threads.
   * Active credit obligations are rejected by server_destroy before this call. */
  if (owner->handoff.impl != NULL) (void)cnet_handoff_destroy(&owner->handoff);
  if (owner->websocket_commands != NULL)
    for (index = 0u; index < server->config.network.command_capacity; ++index)
      chttp_server_buffer_release(server, owner->websocket_commands[index].data,
                                  owner->websocket_commands[index].size);
  if (owner->websocket_pending != NULL)
    for (index = 0u; index < owner->websocket_pending_count; ++index)
      chttp_server_buffer_release(server, owner->websocket_pending[index].data,
                                  owner->websocket_pending[index].size);
  free(owner->file_transfers);
  free(owner->websocket_commands);
  free(owner->websocket_pending);
  owner->file_transfers = NULL;
  owner->websocket_commands = NULL;
  owner->websocket_pending = NULL;
  owner->file_transfer_capacity = 0u;
  owner->websocket_command_count = 0u;
  owner->websocket_command_claims = 0u;
  owner->websocket_pending_count = 0u;
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
  owner->websocket_pending = (chttp_server_websocket_command *)calloc(
      server->config.network.command_capacity, sizeof(*owner->websocket_pending));
  if (owner->file_transfers == NULL ||
      owner->websocket_commands == NULL || owner->websocket_pending == NULL) {
    chttp_server_owner_storage_release(server, owner);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int chttp_server_owner_topology_configure(
    chttp_server_impl *server, size_t owner_count) {
  chttp_server_owner_lane primary = {0};
  chttp_server_owner_lane *additional = NULL;
  cnet_owner_placement_hint *placement_hints;
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
  if (owner_count > SIZE_MAX / sizeof(*placement_hints)) {
    free(additional);
    return SALTS_ERANGE;
  }
  placement_hints = (cnet_owner_placement_hint *)calloc(
      owner_count, sizeof(*placement_hints));
  if (placement_hints == NULL) {
    free(additional);
    return SALTS_ENOMEM;
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
  free(server->acceptor.placement_hints);

  server->owner = primary;
  primary = (chttp_server_owner_lane){0};
  server->additional_owners = additional;
  server->acceptor.placement_hints = placement_hints;
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
  free(placement_hints);
  return status;
}

void chttp_server_impl_free(chttp_server_impl *impl) {
  size_t index;
  if (impl == NULL) return;
  if (impl->connections != NULL)
    for (index = 0u; index < impl->config.network.connection_capacity; ++index)
      chttp_server_connection_destroy(&impl->connections[index]);
  chttp_session_store_destroy(impl);
  for (index = 0u; index < impl->owner_count; ++index)
    chttp_server_owner_storage_release(impl, chttp_server_owner_at(impl, index));
  free(impl->additional_owners);
  free(impl->acceptor.placement_hints);
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
  impl->owner_placement_options =
      (chttp_server_owner_placement_options)CHTTP_SERVER_OWNER_PLACEMENT_OPTIONS_INIT;
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
  status = chttp_server_owner_topology_configure(impl, 1u);
  if (status != SALTS_OK) {
    chttp_server_impl_free(impl);
    return status;
  }
  if (impl->host == NULL || impl->session_cookie_name == NULL || impl->routes == NULL ||
      impl->route_paths == NULL ||
      (route_middleware_count != 0u && impl->route_middleware == NULL) ||
      (config->middleware_capacity != 0u && impl->middleware == NULL) ||
      impl->connections == NULL || impl->acceptor.placement_hints == NULL ||
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
      impl->acceptor.initialized)
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
  if (impl->owner_placement_options.kind == CNET_OWNER_PLACE_EXPLICIT &&
      impl->owner_placement_options.explicit_owner >= options->owner_count)
    return SALTS_EINVAL;
  if (impl->start_called || impl->thread_started || impl->network_initialized ||
      impl->acceptor.initialized)
    return SALTS_EBUSY;
  if (options->owner_count != impl->owner_count) {
    const int status =
        chttp_server_owner_topology_configure(impl, options->owner_count);
    if (status != SALTS_OK) return status;
  }
  impl->execution_options = *options;
  return SALTS_OK;
}

int chttp_server_set_owner_placement(
    chttp_server *server, const chttp_server_owner_placement_options *options) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || options == NULL ||
      options->size != sizeof(*options) ||
      options->version != CHTTP_SERVER_OWNER_PLACEMENT_OPTIONS_VERSION)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called || impl->thread_started || impl->network_initialized ||
      impl->acceptor.initialized)
    return SALTS_EBUSY;
  switch (options->kind) {
  case CNET_OWNER_PLACE_ROUND_ROBIN:
  case CNET_OWNER_PLACE_LOWEST_PRESSURE:
    if (options->explicit_owner != 0u) return SALTS_EINVAL;
    break;
  case CNET_OWNER_PLACE_EXPLICIT:
    if (options->explicit_owner >= impl->owner_count) return SALTS_EINVAL;
    break;
  default:
    /* STRICT_KEY cannot use an HTTP request key during TCP admission:
     * it must fail fast instead of pretending to hash an unknown key. */
    return SALTS_EINVAL;
  }
  impl->owner_placement_options = *options;
  return SALTS_OK;
}

int chttp_server_use_jwt_bearer(chttp_server *server,
                                chttp_jwt_bearer_validator *validator) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || validator == NULL || validator->impl == NULL)
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  if (impl->jwt_bearer_validator != NULL) return SALTS_EALREADY;
  impl->jwt_bearer_validator = validator;
  return SALTS_OK;
}

int chttp_server_set_websocket_transport(
    chttp_server *server, const chttp_server_websocket_transport_options *options) {
  chttp_server_impl *impl;
  if (server == NULL || server->impl == NULL || options == NULL ||
      options->size != sizeof(*options) ||
      options->version != CHTTP_SERVER_WEBSOCKET_TRANSPORT_OPTIONS_VERSION ||
      (options->dedicated_h1 != 0 && options->dedicated_h1 != 1))
    return SALTS_EINVAL;
  impl = (chttp_server_impl *)server->impl;
  if (impl->start_called) return SALTS_EBUSY;
  impl->websocket_dedicated_h1 = options->dedicated_h1 != 0;
  return SALTS_OK;
}
