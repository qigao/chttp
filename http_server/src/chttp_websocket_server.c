#include "chttp_server_runtime.h"
#include "chttp_h2_server.h"
#include "chttp_websocket_handshake.h"
#include "chttp_cnet_retained.h"

#include <salts/clock.h>

#include <stdlib.h>
#include <string.h>

static void chttp_server_websocket_profile_add(
    _Atomic uint64_t *counter, uint64_t value) {
  if (counter != NULL && value != 0u)
    atomic_fetch_add_explicit(counter, value, memory_order_relaxed);
}

static void chttp_server_websocket_profile_max(
    _Atomic uint64_t *counter, uint64_t value) {
  uint64_t observed;
  if (counter == NULL) return;
  observed = atomic_load_explicit(counter, memory_order_relaxed);
  while (observed < value &&
         !atomic_compare_exchange_weak_explicit(
             counter, &observed, value, memory_order_relaxed,
             memory_order_relaxed)) {
  }
}

void chttp_server_websocket_profile_reset(
    chttp_server_websocket_profile *profile) {
  if (profile == NULL) return;
  atomic_init(&profile->commands, 0u);
  atomic_init(&profile->bytes, 0u);
  atomic_init(&profile->failed_commands, 0u);
  atomic_init(&profile->copy_ns, 0u);
  atomic_init(&profile->enqueue_ns, 0u);
  atomic_init(&profile->wake_ns, 0u);
  atomic_init(&profile->queue_residence_ns, 0u);
  atomic_init(&profile->send_admission_ns, 0u);
  atomic_init(&profile->send_completion_samples, 0u);
  atomic_init(&profile->send_completion_ns, 0u);
  atomic_init(&profile->max_queue_residence_ns, 0u);
  atomic_init(&profile->max_send_admission_ns, 0u);
  atomic_init(&profile->max_send_completion_ns, 0u);
}

void chttp_server_websocket_profile_send_complete(
    chttp_server_connection *connection) {
  chttp_server_websocket_profile *profile;
  uint64_t elapsed;
  if (connection == NULL || connection->websocket_send_profile == NULL ||
      connection->websocket_send_started_ns == 0u)
    return;
  profile = connection->websocket_send_profile;
  elapsed = cmeta_hrtime() - connection->websocket_send_started_ns;
  atomic_fetch_add_explicit(&profile->send_completion_samples, 1u,
                            memory_order_relaxed);
  chttp_server_websocket_profile_add(&profile->send_completion_ns, elapsed);
  chttp_server_websocket_profile_max(&profile->max_send_completion_ns,
                                     elapsed);
  connection->websocket_send_profile = NULL;
  connection->websocket_send_started_ns = 0u;
}

typedef struct chttp_websocket_open_context {
  chttp_server_websocket_peer *peer;
  chttp_server_route_record *route;
  bool called;
} chttp_websocket_open_context;

static int chttp_server_websocket_apply(chttp_server_websocket_peer *peer,
    chttp_server_websocket_command_kind kind, uint16_t code, const void *data, size_t size);

static void chttp_server_websocket_tag_complete(void *user, cnet_websocket *websocket,
                                                uint64_t tag, size_t size, int status) {
  chttp_server_websocket_peer *peer = (chttp_server_websocket_peer *)user;
  chttp_server_connection *connection = (chttp_server_connection *)peer->transport;
  if (websocket != peer->session || !peer->tag_pending || tag != 1u || size != peer->tag_size)
    status = SALTS_EPROTO;
  peer->tag_pending = false;
  if (status == SALTS_OK) chttp_server_websocket_profile_send_complete(connection);
  else chttp_server_connection_close(connection);
}

static chttp_server_websocket_peer *chttp_websocket_peer(const chttp_websocket *websocket) {
  chttp_server_websocket_peer *peer;
  if (websocket == NULL || websocket->impl == NULL) return NULL;
  peer = (chttp_server_websocket_peer *)websocket->impl;
  if (&peer->handle != websocket || peer->phase == CHTTP_SERVER_WEBSOCKET_NONE ||
      peer->session == NULL)
    return NULL;
  return peer;
}

int chttp_server_websocket_profile_callback_send_begin(
    chttp_websocket *websocket, chttp_server_websocket_profile *profile) {
  chttp_server_websocket_peer *peer;
  chttp_server_connection *connection;
  if (websocket == NULL || profile == NULL) return SALTS_EINVAL;
  peer = chttp_websocket_peer(websocket);
  if (peer == NULL || peer->stream_id != 0 || peer->transport == NULL)
    return SALTS_ENOTSUP;
  connection = (chttp_server_connection *)peer->transport;
  if (connection->websocket_send_profile != NULL ||
      connection->websocket_send_started_ns != 0u)
    return SALTS_EBUSY;
  connection->websocket_send_profile = profile;
  connection->websocket_send_started_ns = cmeta_hrtime();
  return SALTS_OK;
}

static int chttp_server_websocket_engine_write(void *user, const uint8_t *data, size_t size) {
  chttp_server_websocket_peer *peer = (chttp_server_websocket_peer *)user;
  if (peer == NULL || peer->write == NULL || peer->phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
  return peer->write(peer->transport, data, size);
}

static int chttp_server_websocket_h1_write(void *transport, const uint8_t *data, size_t size) {
  chttp_server_connection *connection = (chttp_server_connection *)transport;
  int status;
  if (connection == NULL || data == NULL || size == 0u ||
      connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
  if (connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_HANDSHAKE || connection->writing ||
      connection->outbound_size != 0u || connection->pending_action == CHTTP_SERVER_PENDING_SEND)
    return SALTS_EBUSY;
  if (size > connection->server->config.network.max_send_bytes) return SALTS_EMSGSIZE;
  status = chttp_server_connection_reserve_outbound(connection, size);
  if (status != SALTS_OK) return status;
  memcpy(connection->outbound, data, size);
  connection->outbound_size = size;
  status = chttp_server_send_pending(connection);
  if (status == SALTS_OK || status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
  connection->outbound_size = 0u;
  return status;
}

static void chttp_server_websocket_event(void *user, cnet_websocket *websocket,
                                         const cnet_websocket_event *event) {
  chttp_server_websocket_peer *peer = (chttp_server_websocket_peer *)user;
  chttp_server_impl *previous_callback_server;
  chttp_websocket_event public_event;
  (void)websocket;
  if (peer == NULL || event == NULL || peer->route == NULL || peer->route->websocket_event == NULL)
    return;
  public_event =
      (chttp_websocket_event){.kind = (chttp_websocket_event_kind)event->kind,
                              .message_type = (chttp_websocket_message_type)event->message_type,
                              .data = event->data,
                              .size = event->size,
                              .close_code = event->close_code};
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = peer->server;
  peer->route->websocket_event(peer->route->websocket_user, &peer->handle, &public_event);
  chttp_active_callback_server = previous_callback_server;
}

int chttp_server_websocket_peer_init(chttp_server_websocket_peer *peer, chttp_server_impl *server,
                                     chttp_server_route_record *route, cnet_connection connection,
                                     uint32_t server_slot, uint32_t server_generation,
                                     int32_t stream_id,
                                     chttp_server_websocket_write_fn write, void *transport) {
  cnet_websocket_config config;
  int status;
  if (peer == NULL || server == NULL || route == NULL || connection.slot == 0u ||
      connection.generation == 0u || server_slot == 0u || server_generation == 0u ||
      stream_id < 0 || write == NULL || transport == NULL)
    return SALTS_EINVAL;
  if (peer->engine.impl != NULL || peer->phase != CHTTP_SERVER_WEBSOCKET_NONE) return SALTS_EBUSY;
  config =
      (cnet_websocket_config){.size = sizeof(config),
                              .role = CNET_WEBSOCKET_SERVER,
                              .max_frame_bytes = route->websocket_max_frame_bytes,
                              .max_message_bytes = route->websocket_max_message_bytes,
                              .max_buffered_input_bytes = route->websocket_max_buffered_input_bytes,
                              .write = chttp_server_websocket_engine_write,
                              .on_event = chttp_server_websocket_event,
                              .user = peer};
  status = cnet_websocket_init(&peer->engine, &config);
  if (status == SALTS_OK) peer->session = &peer->engine;
  if (status != SALTS_OK) return status;
  peer->handle.impl = peer;
  peer->server = server;
  peer->route = route;
  peer->connection = connection;
  peer->server_slot = server_slot;
  peer->server_generation = server_generation;
  peer->stream_id = stream_id;
  peer->dedicated = stream_id == 0 && server->websocket_dedicated_h1;
  peer->write = write;
  peer->transport = transport;
  peer->phase = CHTTP_SERVER_WEBSOCKET_HANDSHAKE;
  return SALTS_OK;
}

void chttp_server_websocket_peer_reset(chttp_server_websocket_peer *peer) {
  if (peer == NULL) return;
  if (peer->bridge.impl != NULL) {
    size_t events = 0u;
    /* Terminal may already have settled native I/O while its logical callback
     * still needs owner progress. Failure to destroy retains the whole peer;
     * retry_pending, Manager retirement and stop all respect that obligation. */
    (void)cnet_websocket_transport_advance(&peer->bridge, 1u, &events);
    if (cnet_websocket_transport_destroy(&peer->bridge) != SALTS_OK) return;
  }
  if (peer->engine.impl != NULL) (void)cnet_websocket_destroy(&peer->engine);
  if (peer->output_retained != NULL) mem_buffer_release(peer->output_retained);
  chttp_server_buffer_release(peer->server, peer->output, peer->output_capacity);
  chttp_server_buffer_release(peer->server, peer->opening_data, peer->opening_capacity);
  *peer = (chttp_server_websocket_peer){0};
}

void chttp_server_websocket_peer_open(chttp_server_websocket_peer *peer) {
  if (peer != NULL && peer->phase == CHTTP_SERVER_WEBSOCKET_HANDSHAKE)
    peer->phase = CHTTP_SERVER_WEBSOCKET_OPEN;
}

int chttp_server_websocket_peer_feed(chttp_server_websocket_peer *peer, const void *data,
                                     size_t size) {
  if (peer == NULL || peer->phase != CHTTP_SERVER_WEBSOCKET_OPEN || peer->session == NULL)
    return SALTS_EINVAL;
  return cnet_websocket_feed(peer->session, data, size);
}

int chttp_server_websocket_peer_flush(chttp_server_websocket_peer *peer) {
  if (peer == NULL || peer->phase == CHTTP_SERVER_WEBSOCKET_NONE || peer->session == NULL)
    return SALTS_EINVAL;
  if (peer->bridge.impl != NULL) {
    size_t events = 0u;
    return cnet_websocket_transport_advance(&peer->bridge, 1u, &events);
  }
  return cnet_websocket_flush(peer->session);
}

void chttp_server_websocket_peer_transport_closed(chttp_server_websocket_peer *peer) {
  if (peer == NULL || peer->session == NULL) return;
  if (peer->bridge.impl == NULL) (void)cnet_websocket_transport_closed(peer->session);
}

bool chttp_server_websocket_peer_terminal(const chttp_server_websocket_peer *peer) {
  cnet_websocket_state state = CNET_WEBSOCKET_FAILED;
  return peer == NULL || peer->session == NULL ||
         (cnet_websocket_state_get(peer->session, &state) == SALTS_OK &&
          (state == CNET_WEBSOCKET_CLOSED || state == CNET_WEBSOCKET_FAILED));
}

void chttp_server_websocket_reset(chttp_server_connection *connection) {
  if (connection == NULL) return;
  chttp_server_websocket_peer_reset(&connection->websocket_peer);
  connection->websocket_upgrade_input_size = 0u;
}

static int chttp_server_websocket_open(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response) {
  chttp_websocket_open_context *context = (chttp_websocket_open_context *)user;
  context->called = true;
  return context->route->websocket_open(context->route->websocket_user, &context->peer->handle,
                                        request, response);
}

int chttp_server_websocket_route_open(chttp_server_websocket_peer *peer,
                                      chttp_server_request_state *state,
                                      chttp_server_route_record *route,
                                      const chttp_server_request_view *request) {
  chttp_server_request_view routed_request;
  chttp_server_chain chain;
  chttp_websocket_open_context open_context;
  chttp_server_impl *previous_callback_server;
  int status;
  if (peer == NULL || state == NULL || route == NULL || request == NULL || peer->server == NULL ||
      peer->phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
  if (!state->admission_complete) chttp_server_stats_request(peer->server);
  routed_request = *request;
  routed_request.params = state->params;
  routed_request.param_count = state->param_count;
  routed_request.session = peer->server->config.session_capacity == 0u ? NULL : &state->session;
  routed_request.jwt_claims = state->jwt_owner != NULL ? &state->jwt_claims : NULL;
  chttp_session_request_begin(state, &routed_request);
  chttp_server_response_builder_reset(&state->response_builder);
  open_context = (chttp_websocket_open_context){.peer = peer, .route = route};
  chain = (chttp_server_chain){.server = peer->server,
                               .request_state = state,
                               .request = &routed_request,
                               .response = &state->response,
                               .route = route,
                               .terminal = chttp_server_websocket_open,
                               .terminal_user = &open_context};
  previous_callback_server = chttp_active_callback_server;
  chttp_active_callback_server = peer->server;
  status = chttp_server_chain_run(&chain);
  chttp_active_callback_server = previous_callback_server;
  if (status == SALTS_OK && !open_context.called && !state->response_builder.replied)
    status = SALTS_EPROTO;
  if (status == SALTS_OK) status = chttp_session_request_finish(state);
  if (status != SALTS_OK) {
    chttp_session_request_abort(state);
    chttp_server_stats_handler_error(peer->server);
  }
  return status;
}

static int chttp_server_websocket_append(unsigned char *output, size_t capacity, size_t *size,
                                         const void *data, size_t data_size) {
  if (*size > capacity || data_size > capacity - *size) return SALTS_EMSGSIZE;
  if (data_size != 0u) memcpy(output + *size, data, data_size);
  *size += data_size;
  return SALTS_OK;
}

static int chttp_server_websocket_handshake_serialize(const chttp_server_response_builder *builder,
                                                      const char *accept, unsigned char *output,
                                                      size_t output_capacity, size_t *out_size) {
  static const char prefix[] = "HTTP/1.1 101 Switching Protocols\r\n";
  static const char required_prefix[] =
      "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
  size_t size = 0u;
  size_t index;
  int status;
  if (builder == NULL || accept == NULL || output == NULL || out_size == NULL) return SALTS_EINVAL;
  status =
      chttp_server_websocket_append(output, output_capacity, &size, prefix, sizeof(prefix) - 1u);
  for (index = 0u; status == SALTS_OK && index < builder->header_count; ++index) {
    const chttp_header *header = &builder->headers[index];
    status = chttp_server_websocket_append(output, output_capacity, &size, header->name,
                                           strlen(header->name));
    if (status == SALTS_OK)
      status = chttp_server_websocket_append(output, output_capacity, &size, ": ", 2u);
    if (status == SALTS_OK)
      status = chttp_server_websocket_append(output, output_capacity, &size, header->value,
                                             strlen(header->value));
    if (status == SALTS_OK)
      status = chttp_server_websocket_append(output, output_capacity, &size, "\r\n", 2u);
  }
  if (status == SALTS_OK)
    status = chttp_server_websocket_append(output, output_capacity, &size, required_prefix,
                                           sizeof(required_prefix) - 1u);
  if (status == SALTS_OK)
    status = chttp_server_websocket_append(output, output_capacity, &size, accept,
                                           CHTTP_WEBSOCKET_ACCEPT_BYTES);
  if (status == SALTS_OK)
    status = chttp_server_websocket_append(output, output_capacity, &size, "\r\n\r\n", 4u);
  if (status == SALTS_OK) *out_size = size;
  return status;
}

int chttp_server_websocket_upgrade(void *user, const chttp_server_request_view *request,
                                   chttp_server_parser_upgrade_action *out_action,
                                   unsigned int *out_http_status) {
  chttp_server_connection *connection = (chttp_server_connection *)user;
  chttp_server_request_view enriched_request;
  chttp_server_request_state *state;
  chttp_server_route_record *route;
  char accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
  int status;
  if (connection == NULL || request == NULL || out_action == NULL || out_http_status == NULL)
    return SALTS_EINVAL;
  *out_action = CHTTP_SERVER_UPGRADE_IGNORE;
  *out_http_status = 0u;
  enriched_request = *request;
  chttp_server_request_enrich(connection, &enriched_request);
  request = &enriched_request;
  state = &connection->request_state;
  if (!state->admission_complete || state->admission_rejected) return SALTS_EPERM;
  route = state->admitted_route;
  if (route == NULL || !route->websocket) return SALTS_OK;
  status =
      chttp_websocket_server_handshake_validate(request, accept, sizeof(accept), out_http_status);
  if (status != SALTS_OK) return status;
  if (connection->outbound_size > connection->server->config.network.max_send_bytes ||
      connection->server->max_response_wire_bytes >
          connection->server->config.network.max_send_bytes - connection->outbound_size)
    return SALTS_ENOBUFS;
  status = chttp_server_connection_reserve_outbound(
      connection, connection->outbound_size + connection->server->max_response_wire_bytes);
  if (status != SALTS_OK) return status;
  status = chttp_server_websocket_peer_init(&connection->websocket_peer, connection->server, route,
                                            connection->handle, connection->server_slot,
                                            connection->server_generation, 0,
                                            chttp_server_websocket_h1_write, connection);
  if (status != SALTS_OK) return status;
  status = chttp_server_websocket_route_open(&connection->websocket_peer, state, route, request);
  if (status != SALTS_OK) {
    chttp_server_websocket_reset(connection);
    *out_http_status = 500u;
    return status;
  }
  if (state->response_builder.replied) {
    if (state->response_builder.source_enabled) {
      chttp_server_response_builder_close_source(&state->response_builder, SALTS_ENOTSUP);
      chttp_server_websocket_reset(connection);
      *out_http_status = 500u;
      return SALTS_ENOTSUP;
    }
    status =
        chttp_server_response_serialize(&state->response_builder, request, connection->outbound,
                                        connection->outbound_capacity, &connection->outbound_size);
    chttp_server_websocket_reset(connection);
    if (status != SALTS_OK) {
      *out_http_status = 500u;
      return status;
    }
    connection->close_after_write = true;
  } else {
    status = chttp_server_websocket_handshake_serialize(
        &state->response_builder, accept, connection->outbound, connection->outbound_capacity,
        &connection->outbound_size);
    if (status != SALTS_OK) {
      chttp_server_websocket_reset(connection);
      *out_http_status = 500u;
      return status;
    }
  }
  chttp_server_stats_response(connection->server);
  *out_action = CHTTP_SERVER_UPGRADE_STOP;
  return SALTS_OK;
}

int chttp_server_websocket_input(chttp_server_connection *connection, const void *data,
                                 size_t size) {
  int status;
  if (connection == NULL || connection->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_OPEN)
    return SALTS_EINVAL;
  status = chttp_server_websocket_peer_feed(&connection->websocket_peer, data, size);
  if (status != SALTS_EBUSY) return status;
  if (!cnet_websocket_has_pending_output(connection->websocket_peer.session)) return status;
  if (connection->websocket_upgrade_input_size != 0u) return SALTS_ENOBUFS;
  status = chttp_server_buffer_grow(connection->server, &connection->websocket_upgrade_input,
                                    &connection->websocket_upgrade_input_capacity, size,
                                    connection->server->config.network.receive_buffer_bytes, 0u);
  if (status != SALTS_OK) return status;
  if (size != 0u) memcpy(connection->websocket_upgrade_input, data, size);
  connection->websocket_upgrade_input_size = size;
  return SALTS_OK;
}

static int chttp_server_websocket_bind(chttp_server_connection *connection) {
  chttp_server_websocket_peer *peer = &connection->websocket_peer;
  const size_t capacity = peer->route->websocket_max_frame_bytes + CNET_WEBSOCKET_MAX_HEADER_BYTES;
  const cnet_websocket_tagged_policy policy = {
      .size = sizeof(policy), .version = CNET_WEBSOCKET_TAGGED_SEND_VERSION,
      .fragment_bytes = peer->route->websocket_max_frame_bytes,
      .on_send = chttp_server_websocket_tag_complete, .user = peer};
  cnet_websocket_config config = {
      .size = sizeof(config), .role = CNET_WEBSOCKET_SERVER,
      .max_frame_bytes = peer->route->websocket_max_frame_bytes,
      .max_message_bytes = peer->route->websocket_max_message_bytes,
      .max_buffered_input_bytes = peer->route->websocket_max_buffered_input_bytes,
      .on_event = chttp_server_websocket_event, .user = peer};
  int status = chttp_server_buffer_grow(peer->server, &peer->output, &peer->output_capacity,
                                        capacity, capacity, 0u);
  if (status != SALTS_OK) return status;
  status = chttp_cnet_retained_bind(&peer->output_retained, peer->output, peer->output_capacity);
  if (status != SALTS_OK) return status;
  config.output_buffer = peer->output_retained;
  status = cnet_websocket_transport_init(&peer->bridge, chttp_server_connection_network(connection),
                                          connection->handle, &config, &policy);
  if (status != SALTS_OK) return status;
  /* The opening engine has never written to the wire. Transfer its single
   * accepted operation from owned input after the HTTP write settles. */
  status = cnet_websocket_destroy(&peer->engine);
  if (status != SALTS_OK) return status;
  status = cnet_websocket_transport_session(&peer->bridge, &peer->session);
  if (status != SALTS_OK) return status;
  peer->phase = CHTTP_SERVER_WEBSOCKET_OPEN;
  if (peer->opening_pending) {
    status = chttp_server_websocket_apply(peer,
        (chttp_server_websocket_command_kind)peer->opening_kind, peer->opening_code,
        peer->opening_data, peer->opening_size);
    if (status != SALTS_OK) return status;
    peer->opening_pending = false;
  }
  chttp_server_buffer_release(peer->server, peer->opening_data, peer->opening_capacity);
  peer->opening_data = NULL;
  peer->opening_capacity = 0u;
  return SALTS_OK;
}

int chttp_server_websocket_progress(chttp_server_connection *connection) {
  chttp_server_websocket_peer *peer = &connection->websocket_peer;
  int status;
  if (peer->bridge.impl == NULL) return SALTS_OK;
  status = chttp_server_websocket_peer_flush(peer);
  if (status != SALTS_OK) return status;
  if (peer->tag_pending || cnet_websocket_has_pending_output(peer->session)) return SALTS_OK;
  chttp_server_websocket_profile_send_complete(connection);
  if (connection->websocket_upgrade_input_size != 0u) {
    status = chttp_server_websocket_peer_feed(peer, connection->websocket_upgrade_input,
                                              connection->websocket_upgrade_input_size);
    if (status == SALTS_EBUSY) return SALTS_OK;
    if (status != SALTS_OK) return status;
    connection->websocket_upgrade_input_size = 0u;
    chttp_server_buffer_release(connection->server, connection->websocket_upgrade_input,
                                connection->websocket_upgrade_input_capacity);
    connection->websocket_upgrade_input = NULL;
    connection->websocket_upgrade_input_capacity = 0u;
  }
  if (peer->tag_pending || cnet_websocket_has_pending_output(peer->session)) return SALTS_OK;
  if (chttp_server_websocket_peer_terminal(peer)) {
    chttp_server_connection_close(connection);
  } else if (peer->receive_paused && !connection->close_after_write) {
    peer->receive_paused = false;
    status = chttp_server_send_pending(connection);
    if (status != SALTS_OK && status != SALTS_EBUSY && status != SALTS_ENOBUFS) return status;
  }
  return SALTS_OK;
}

int chttp_server_websocket_send_complete(chttp_server_connection *connection) {
  int status;
  if (connection == NULL || connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
  if (connection->websocket_peer.dedicated) {
    status = chttp_server_websocket_bind(connection);
    if (status != SALTS_OK) return status;
    return chttp_server_websocket_progress(connection);
  }
  chttp_server_websocket_peer_open(&connection->websocket_peer);
  status = chttp_server_websocket_peer_flush(&connection->websocket_peer);
  if (status != SALTS_OK) return status;
  if (connection->websocket_upgrade_input_size != 0u) {
    const size_t size = connection->websocket_upgrade_input_size;
    connection->websocket_upgrade_input_size = 0u;
    status = chttp_server_websocket_peer_feed(&connection->websocket_peer,
                                              connection->websocket_upgrade_input, size);
    chttp_server_buffer_release(connection->server, connection->websocket_upgrade_input,
                                connection->websocket_upgrade_input_capacity);
    connection->websocket_upgrade_input = NULL;
    connection->websocket_upgrade_input_capacity = 0u;
  }
  if (status == SALTS_OK && chttp_server_websocket_peer_terminal(&connection->websocket_peer) &&
      !connection->writing && connection->outbound_size == 0u &&
      connection->pending_action == CHTTP_SERVER_PENDING_NONE)
    chttp_server_connection_close(connection);
  return status;
}

void chttp_server_websocket_transport_closed(chttp_server_connection *connection) {
  if (connection == NULL) return;
  connection->websocket_send_profile = NULL;
  connection->websocket_send_started_ns = 0u;
  chttp_server_websocket_peer_transport_closed(&connection->websocket_peer);
  chttp_server_websocket_reset(connection);
}

int chttp_websocket_state_get(const chttp_websocket *websocket, chttp_websocket_state *out_state) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  cnet_websocket_state state;
  int status;
  if (peer == NULL || out_state == NULL) return SALTS_EINVAL;
  status = cnet_websocket_state_get(peer->session, &state);
  if (status == SALTS_OK) *out_state = (chttp_websocket_state)state;
  return status;
}

static int chttp_server_websocket_apply(chttp_server_websocket_peer *peer,
    chttp_server_websocket_command_kind kind, uint16_t code, const void *data, size_t size) {
  const bool opening = peer->dedicated && peer->phase == CHTTP_SERVER_WEBSOCKET_HANDSHAKE;
  int status;
  if (data == NULL && size != 0u) return SALTS_EINVAL;
  if (opening) {
    if (peer->opening_pending) return SALTS_EBUSY;
    if (size > peer->route->websocket_max_frame_bytes) return SALTS_EMSGSIZE;
    if (size != 0u) {
      status = chttp_server_buffer_grow(peer->server, &peer->opening_data,
          &peer->opening_capacity, size, peer->route->websocket_max_frame_bytes, 0u);
      if (status != SALTS_OK) return status;
    }
  }
  if (peer->bridge.impl != NULL &&
      (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT || kind == CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY)) {
    /* Preserve the server's public single-frame bound. Tagged completion owns
     * the one logical admission until native completion and owner advance. */
    if (size > peer->route->websocket_max_frame_bytes) return SALTS_EMSGSIZE;
    status = cnet_websocket_send_tagged(peer->session,
        kind == CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT ? CNET_WEBSOCKET_MESSAGE_TEXT
                                                   : CNET_WEBSOCKET_MESSAGE_BINARY,
        data, size, 1u);
    if (status == SALTS_OK) {
      peer->tag_pending = true;
      peer->tag_size = size;
    }
    return status;
  }
  if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT)
    status = cnet_websocket_send_text(peer->session, data, size);
  else if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY)
    status = cnet_websocket_send_binary(peer->session, data, size);
  else if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PING)
    status = cnet_websocket_send_ping(peer->session, data, size);
  else if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PONG)
    status = cnet_websocket_send_pong(peer->session, data, size);
  else status = cnet_websocket_close(peer->session, code, data, size);
  if (status == SALTS_OK && opening) {
    if (size != 0u) memcpy(peer->opening_data, data, size);
    peer->opening_kind = (int)kind;
    peer->opening_code = code;
    peer->opening_size = size;
    peer->opening_pending = true;
  }
  return status;
}

int chttp_websocket_send_text(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : chttp_server_websocket_apply(
      peer, CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT, 0u, data, size);
}

int chttp_websocket_send_binary(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : chttp_server_websocket_apply(
      peer, CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY, 0u, data, size);
}

int chttp_websocket_send_ping(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : chttp_server_websocket_apply(
      peer, CHTTP_SERVER_WEBSOCKET_COMMAND_PING, 0u, data, size);
}

int chttp_websocket_send_pong(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : chttp_server_websocket_apply(
      peer, CHTTP_SERVER_WEBSOCKET_COMMAND_PONG, 0u, data, size);
}

int chttp_websocket_close(chttp_websocket *websocket, uint16_t code, const void *reason,
                          size_t reason_size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL
                      : chttp_server_websocket_apply(
                          peer, CHTTP_SERVER_WEBSOCKET_COMMAND_CLOSE, code, reason, reason_size);
}

int chttp_server_websocket_session_capture(const chttp_websocket *websocket,
                                            chttp_server_websocket_session *out_session) {
  chttp_server_websocket_peer *peer;
  if (out_session == NULL) return SALTS_EINVAL;
  *out_session = (chttp_server_websocket_session){0};
  peer = chttp_websocket_peer(websocket);
  if (peer == NULL || chttp_active_callback_server != peer->server) return SALTS_EINVAL;
  *out_session = (chttp_server_websocket_session){.impl = peer->server,
                                                  .connection_slot = peer->server_slot,
                                                  .connection_generation =
                                                      peer->server_generation,
                                                  .stream_id = peer->stream_id};
  return SALTS_OK;
}

static chttp_server_owner_lane *chttp_server_websocket_command_owner(
    chttp_server_impl *server, const chttp_server_websocket_session *session) {
  size_t index;
  if (server == NULL || session == NULL || session->connection_slot == 0u)
    return NULL;
  index = (size_t)session->connection_slot - 1u;
  if (index >= server->config.network.connection_capacity) return NULL;
  return server->connections[index].owner;
}

static int chttp_server_websocket_command_submit(
    const chttp_server_websocket_session *session, chttp_server_websocket_command_kind kind,
    uint16_t close_code, const void *data, size_t size) {
  chttp_server_impl *server;
  chttp_server_websocket_command *command;
  chttp_server_websocket_profile *profile;
  chttp_server_owner_lane *owner;
  unsigned char *copy = NULL;
  size_t copy_capacity = 0u;
  uint64_t copy_started = 0u;
  uint64_t enqueue_started = 0u;
  uint64_t wake_started = 0u;
  int status = SALTS_OK;
  if (session == NULL || session->impl == NULL || session->connection_slot == 0u ||
      session->connection_generation == 0u || session->stream_id < 0 ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  server = (chttp_server_impl *)session->impl;
  profile = server->websocket_profile;
  owner = chttp_server_websocket_command_owner(server, session);
  if (owner == NULL || owner->server != server)
    return SALTS_EINVAL;
  if (size > server->config.network.max_send_bytes) return SALTS_EMSGSIZE;
  if ((kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PING ||
       kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PONG) &&
      size > CNET_WEBSOCKET_MAX_CONTROL_BYTES)
    return SALTS_EMSGSIZE;
  if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_CLOSE &&
      size > CNET_WEBSOCKET_MAX_CONTROL_BYTES - sizeof(uint16_t))
    return SALTS_EMSGSIZE;
  /* Reserve before allocating: copied, queued and retrying payloads share the
   * same hard slot bound. Keep the claim until the last use of server/profile
   * so destroy cannot overtake a producer copying outside the lifecycle lock. */
  cmeta_mutex_lock(&server->mutex);
  if (!server->stats.running || server->stats.stopping || server->worker_done)
    status = SALTS_ESHUTDOWN;
  else if (owner->websocket_command_claims >= server->config.network.command_capacity -
               owner->websocket_command_count - owner->websocket_pending_count)
    status = SALTS_ENOBUFS;
  if (status != SALTS_OK) {
    if (profile != NULL)
      atomic_fetch_add_explicit(&profile->failed_commands, 1u, memory_order_relaxed);
    cmeta_mutex_unlock(&server->mutex);
    return status;
  }
  ++owner->websocket_command_claims;
  cmeta_mutex_unlock(&server->mutex);
  if (profile != NULL) copy_started = cmeta_hrtime();
  if (size != 0u) {
    status = chttp_server_buffer_grow(server, &copy, &copy_capacity, size, size, 0u);
    if (status == SALTS_OK) memcpy(copy, data, size);
  }
  if (profile != NULL)
    chttp_server_websocket_profile_add(
        &profile->copy_ns, cmeta_hrtime() - copy_started);
  if (profile != NULL) enqueue_started = cmeta_hrtime();
  cmeta_mutex_lock(&server->mutex);
  if (status == SALTS_OK &&
      (!server->stats.running || server->stats.stopping || server->worker_done))
    status = SALTS_ESHUTDOWN;
  if (status != SALTS_OK) {
    cmeta_mutex_unlock(&server->mutex);
    if (profile != NULL) {
      chttp_server_websocket_profile_add(
          &profile->enqueue_ns, cmeta_hrtime() - enqueue_started);
      atomic_fetch_add_explicit(&profile->failed_commands, 1u,
                                memory_order_relaxed);
    }
    chttp_server_buffer_release(server, copy, copy_capacity);
    cmeta_mutex_lock(&server->mutex);
    --owner->websocket_command_claims;
    cmeta_mutex_unlock(&server->mutex);
    return status;
  }
  command = &owner->websocket_commands[owner->websocket_command_count];
  *command = (chttp_server_websocket_command){
      .session = *session,
      .data = copy,
      .profile = profile,
      .profile_enqueued_ns = profile != NULL ? cmeta_hrtime() : 0u,
      .size = size,
      .close_code = close_code,
      .kind = kind};
  ++owner->websocket_command_count;
  --owner->websocket_command_claims;
  if (profile != NULL) {
    chttp_server_websocket_profile_add(
        &profile->enqueue_ns, cmeta_hrtime() - enqueue_started);
    atomic_fetch_add_explicit(&profile->commands, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&profile->bytes, (uint64_t)size,
                              memory_order_relaxed);
    wake_started = cmeta_hrtime();
  }
  (void)chttp_server_owner_wake_locked(owner);
  if (profile != NULL)
    chttp_server_websocket_profile_add(
        &profile->wake_ns, cmeta_hrtime() - wake_started);
  cmeta_mutex_unlock(&server->mutex);
  return SALTS_OK;
}

int chttp_server_websocket_send_text(const chttp_server_websocket_session *session,
                                     const void *data, size_t size) {
  return chttp_server_websocket_command_submit(
      session, CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT, 0u, data, size);
}

int chttp_server_websocket_send_binary(const chttp_server_websocket_session *session,
                                       const void *data, size_t size) {
  return chttp_server_websocket_command_submit(
      session, CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY, 0u, data, size);
}

int chttp_server_websocket_send_ping(const chttp_server_websocket_session *session,
                                     const void *data, size_t size) {
  return chttp_server_websocket_command_submit(
      session, CHTTP_SERVER_WEBSOCKET_COMMAND_PING, 0u, data, size);
}

int chttp_server_websocket_send_pong(const chttp_server_websocket_session *session,
                                     const void *data, size_t size) {
  return chttp_server_websocket_command_submit(
      session, CHTTP_SERVER_WEBSOCKET_COMMAND_PONG, 0u, data, size);
}

int chttp_server_websocket_close(const chttp_server_websocket_session *session, uint16_t code,
                                 const void *reason, size_t reason_size) {
  return chttp_server_websocket_command_submit(
      session, CHTTP_SERVER_WEBSOCKET_COMMAND_CLOSE, code, reason, reason_size);
}

static chttp_server_websocket_peer *chttp_server_websocket_command_peer(
    chttp_server_impl *server, const chttp_server_websocket_session *session,
    chttp_server_connection **out_connection) {
  chttp_server_connection *connection;
  const size_t index = (size_t)session->connection_slot - 1u;
  if (out_connection != NULL) *out_connection = NULL;
  if (index >= server->config.network.connection_capacity) return NULL;
  connection = &server->connections[index];
  if (!connection->active || connection->server_slot != session->connection_slot ||
      connection->server_generation != session->connection_generation)
    return NULL;
  if (out_connection != NULL) *out_connection = connection;
  if (session->stream_id == 0)
    return connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE
               ? NULL
               : &connection->websocket_peer;
  return chttp_h2_server_websocket_peer_find(connection->h2, session->stream_id);
}

int chttp_server_websocket_commands_progress(
    chttp_server_impl *server, chttp_server_owner_lane *owner) {
  size_t count;
  size_t kept = 0u;
  if (server == NULL || owner == NULL || owner->server != server ||
      owner->websocket_commands == NULL || owner->websocket_pending == NULL)
    return SALTS_EINVAL;
  /* Snapshot at most command_capacity entries. New publications wait for the
   * next pass, after all older retrying commands. No payload copy, allocation,
   * transport call or callback occurs under the producer lock. */
  cmeta_mutex_lock(&server->mutex);
  memcpy(owner->websocket_pending + owner->websocket_pending_count,
         owner->websocket_commands,
         owner->websocket_command_count * sizeof(*owner->websocket_commands));
  count = owner->websocket_pending_count + owner->websocket_command_count;
  memset(owner->websocket_commands, 0,
         owner->websocket_command_count * sizeof(*owner->websocket_commands));
  owner->websocket_command_count = 0u;
  owner->websocket_pending_count = count;
  cmeta_mutex_unlock(&server->mutex);
  /* Peer-local state avoids a quadratic search for earlier blocked commands,
   * and keeps independent H2 streams independent as well. */
  for (size_t index = 0u; index < count; ++index) {
    chttp_server_websocket_peer *peer = chttp_server_websocket_command_peer(
        server, &owner->websocket_pending[index].session, NULL);
    if (peer != NULL) peer->commands_blocked = false;
  }
  for (size_t index = 0u; index < count; ++index) {
    chttp_server_websocket_command *command = &owner->websocket_pending[index];
    chttp_server_websocket_peer *peer;
    chttp_server_connection *connection = NULL;
    int status;
    if (command->profile != NULL && !command->profile_residence_recorded) {
      const uint64_t residence =
          cmeta_hrtime() - command->profile_enqueued_ns;
      chttp_server_websocket_profile_add(
          &command->profile->queue_residence_ns, residence);
      chttp_server_websocket_profile_max(
          &command->profile->max_queue_residence_ns, residence);
      command->profile_residence_recorded = true;
    }
    peer = chttp_server_websocket_command_peer(server, &command->session, &connection);
    if (peer != NULL && (connection == NULL || connection->owner != owner))
      peer = NULL;
    if (peer == NULL) {
      status = SALTS_ENOENT;
    } else if (peer->commands_blocked) {
      status = SALTS_EBUSY;
    } else {
      const bool profile_h1 =
          command->profile != NULL && command->session.stream_id == 0 &&
          connection != NULL &&
          connection->websocket_send_profile == NULL;
      const uint64_t send_started =
          command->profile != NULL ? cmeta_hrtime() : 0u;
      if (profile_h1) {
        connection->websocket_send_profile = command->profile;
        connection->websocket_send_started_ns = send_started;
      }
      status = chttp_server_websocket_apply(peer, command->kind, command->close_code,
                                            command->data, command->size);
      if (command->profile != NULL)
        command->profile_send_ns += cmeta_hrtime() - send_started;
      if (profile_h1 && status != SALTS_OK) {
        connection->websocket_send_profile = NULL;
        connection->websocket_send_started_ns = 0u;
      }
    }
    if (status == SALTS_OK && command->session.stream_id != 0) {
      status = chttp_server_send_pending(connection);
      if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) status = SALTS_OK;
    }
    if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) {
      peer->commands_blocked = true;
      owner->websocket_pending[kept++] = *command;
      continue;
    }
    if (command->profile != NULL) {
      chttp_server_websocket_profile_add(
          &command->profile->send_admission_ns, command->profile_send_ns);
      chttp_server_websocket_profile_max(
          &command->profile->max_send_admission_ns, command->profile_send_ns);
      if (status != SALTS_OK)
        atomic_fetch_add_explicit(&command->profile->failed_commands, 1u,
                                  memory_order_relaxed);
    }
    chttp_server_buffer_release(server, command->data, command->size);
  }
  memset(owner->websocket_pending + kept, 0,
         (count - kept) * sizeof(*owner->websocket_pending));
  cmeta_mutex_lock(&server->mutex);
  owner->websocket_pending_count = kept;
  cmeta_mutex_unlock(&server->mutex);
  return SALTS_OK;
}
