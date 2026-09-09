#include "chttp_server_runtime.h"
#include "chttp_h2_server.h"
#include "chttp_websocket_handshake.h"

#include <stdlib.h>
#include <string.h>

typedef struct chttp_websocket_open_context {
  chttp_server_websocket_peer *peer;
  chttp_server_route_record *route;
  bool called;
} chttp_websocket_open_context;

static chttp_server_websocket_peer *chttp_websocket_peer(const chttp_websocket *websocket) {
  chttp_server_websocket_peer *peer;
  if (websocket == NULL || websocket->impl == NULL) return NULL;
  peer = (chttp_server_websocket_peer *)websocket->impl;
  if (&peer->handle != websocket || peer->phase == CHTTP_SERVER_WEBSOCKET_NONE ||
      peer->engine.impl == NULL)
    return NULL;
  return peer;
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
                                     int32_t stream_id,
                                     chttp_server_websocket_write_fn write, void *transport) {
  cnet_websocket_config config;
  int status;
  if (peer == NULL || server == NULL || route == NULL || connection.slot == 0u ||
      connection.generation == 0u || stream_id < 0 || write == NULL || transport == NULL)
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
  if (status != SALTS_OK) return status;
  peer->handle.impl = peer;
  peer->server = server;
  peer->route = route;
  peer->connection = connection;
  peer->stream_id = stream_id;
  peer->write = write;
  peer->transport = transport;
  peer->phase = CHTTP_SERVER_WEBSOCKET_HANDSHAKE;
  return SALTS_OK;
}

void chttp_server_websocket_peer_reset(chttp_server_websocket_peer *peer) {
  if (peer == NULL) return;
  if (peer->engine.impl != NULL) (void)cnet_websocket_destroy(&peer->engine);
  *peer = (chttp_server_websocket_peer){0};
}

void chttp_server_websocket_peer_open(chttp_server_websocket_peer *peer) {
  if (peer != NULL && peer->phase == CHTTP_SERVER_WEBSOCKET_HANDSHAKE)
    peer->phase = CHTTP_SERVER_WEBSOCKET_OPEN;
}

int chttp_server_websocket_peer_feed(chttp_server_websocket_peer *peer, const void *data,
                                     size_t size) {
  if (peer == NULL || peer->phase != CHTTP_SERVER_WEBSOCKET_OPEN || peer->engine.impl == NULL)
    return SALTS_EINVAL;
  return cnet_websocket_feed(&peer->engine, data, size);
}

int chttp_server_websocket_peer_flush(chttp_server_websocket_peer *peer) {
  if (peer == NULL || peer->phase == CHTTP_SERVER_WEBSOCKET_NONE || peer->engine.impl == NULL)
    return SALTS_EINVAL;
  return cnet_websocket_flush(&peer->engine);
}

void chttp_server_websocket_peer_transport_closed(chttp_server_websocket_peer *peer) {
  if (peer == NULL || peer->engine.impl == NULL) return;
  (void)cnet_websocket_transport_closed(&peer->engine);
}

bool chttp_server_websocket_peer_terminal(const chttp_server_websocket_peer *peer) {
  cnet_websocket_state state = CNET_WEBSOCKET_FAILED;
  return peer == NULL || peer->engine.impl == NULL ||
         (cnet_websocket_state_get(&peer->engine, &state) == SALTS_OK &&
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
                                            connection->handle, 0,
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
  if (!cnet_websocket_has_pending_output(&connection->websocket_peer.engine)) return status;
  if (connection->websocket_upgrade_input_size != 0u) return SALTS_ENOBUFS;
  status = chttp_server_buffer_grow(connection->server, &connection->websocket_upgrade_input,
                                    &connection->websocket_upgrade_input_capacity, size,
                                    connection->server->config.network.receive_buffer_bytes, 0u);
  if (status != SALTS_OK) return status;
  if (size != 0u) memcpy(connection->websocket_upgrade_input, data, size);
  connection->websocket_upgrade_input_size = size;
  return SALTS_OK;
}

int chttp_server_websocket_send_complete(chttp_server_connection *connection) {
  int status;
  if (connection == NULL || connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
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
  chttp_server_websocket_peer_transport_closed(&connection->websocket_peer);
  chttp_server_websocket_reset(connection);
}

int chttp_websocket_state_get(const chttp_websocket *websocket, chttp_websocket_state *out_state) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  cnet_websocket_state state;
  int status;
  if (peer == NULL || out_state == NULL) return SALTS_EINVAL;
  status = cnet_websocket_state_get(&peer->engine, &state);
  if (status == SALTS_OK) *out_state = (chttp_websocket_state)state;
  return status;
}

int chttp_websocket_send_text(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : cnet_websocket_send_text(&peer->engine, data, size);
}

int chttp_websocket_send_binary(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : cnet_websocket_send_binary(&peer->engine, data, size);
}

int chttp_websocket_send_ping(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : cnet_websocket_send_ping(&peer->engine, data, size);
}

int chttp_websocket_send_pong(chttp_websocket *websocket, const void *data, size_t size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL : cnet_websocket_send_pong(&peer->engine, data, size);
}

int chttp_websocket_close(chttp_websocket *websocket, uint16_t code, const void *reason,
                          size_t reason_size) {
  chttp_server_websocket_peer *peer = chttp_websocket_peer(websocket);
  return peer == NULL ? SALTS_EINVAL
                      : cnet_websocket_close(&peer->engine, code, reason, reason_size);
}

int chttp_server_websocket_session_capture(const chttp_websocket *websocket,
                                            chttp_server_websocket_session *out_session) {
  chttp_server_websocket_peer *peer;
  if (out_session == NULL) return SALTS_EINVAL;
  *out_session = (chttp_server_websocket_session){0};
  peer = chttp_websocket_peer(websocket);
  if (peer == NULL || chttp_active_callback_server != peer->server) return SALTS_EINVAL;
  *out_session = (chttp_server_websocket_session){.impl = peer->server,
                                                  .connection_slot = peer->connection.slot,
                                                  .connection_generation =
                                                      peer->connection.generation,
                                                  .stream_id = peer->stream_id};
  return SALTS_OK;
}

static int chttp_server_websocket_command_submit(
    const chttp_server_websocket_session *session, chttp_server_websocket_command_kind kind,
    uint16_t close_code, const void *data, size_t size) {
  chttp_server_impl *server;
  chttp_server_websocket_command *command;
  unsigned char *copy = NULL;
  size_t tail;
  if (session == NULL || session->impl == NULL || session->connection_slot == 0u ||
      session->connection_generation == 0u || session->stream_id < 0 ||
      (data == NULL && size != 0u))
    return SALTS_EINVAL;
  server = (chttp_server_impl *)session->impl;
  if (size > server->config.network.max_send_bytes) return SALTS_EMSGSIZE;
  if ((kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PING ||
       kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PONG) &&
      size > CNET_WEBSOCKET_MAX_CONTROL_BYTES)
    return SALTS_EMSGSIZE;
  if (kind == CHTTP_SERVER_WEBSOCKET_COMMAND_CLOSE &&
      size > CNET_WEBSOCKET_MAX_CONTROL_BYTES - sizeof(uint16_t))
    return SALTS_EMSGSIZE;
  if (size != 0u) {
    copy = (unsigned char *)malloc(size);
    if (copy == NULL) return SALTS_ENOMEM;
    memcpy(copy, data, size);
  }
  salts_mutex_lock(&server->mutex);
  if (!server->stats.running || server->stats.stopping || server->worker_done) {
    salts_mutex_unlock(&server->mutex);
    free(copy);
    return SALTS_ESHUTDOWN;
  }
  if (server->websocket_command_count == server->config.network.command_capacity) {
    salts_mutex_unlock(&server->mutex);
    free(copy);
    return SALTS_ENOBUFS;
  }
  tail = (server->websocket_command_head + server->websocket_command_count) %
         server->config.network.command_capacity;
  command = &server->websocket_commands[tail];
  *command = (chttp_server_websocket_command){.session = *session,
                                              .data = copy,
                                              .size = size,
                                              .close_code = close_code,
                                              .kind = kind};
  ++server->websocket_command_count;
  salts_mutex_unlock(&server->mutex);
  (void)cnet_client_wake(&server->network);
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
  if (!connection->active || connection->handle.slot != session->connection_slot ||
      connection->handle.generation != session->connection_generation)
    return NULL;
  if (out_connection != NULL) *out_connection = connection;
  if (session->stream_id == 0)
    return connection->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE
               ? NULL
               : &connection->websocket_peer;
  return chttp_h2_server_websocket_peer_find(connection->h2, session->stream_id);
}

int chttp_server_websocket_commands_progress(chttp_server_impl *server) {
  for (;;) {
    chttp_server_websocket_command *command;
    chttp_server_websocket_peer *peer;
    chttp_server_connection *connection = NULL;
    int status;
    salts_mutex_lock(&server->mutex);
    if (server->websocket_command_count == 0u) {
      salts_mutex_unlock(&server->mutex);
      return SALTS_OK;
    }
    command = &server->websocket_commands[server->websocket_command_head];
    salts_mutex_unlock(&server->mutex);
    peer = chttp_server_websocket_command_peer(server, &command->session, &connection);
    if (peer == NULL)
      status = SALTS_ENOENT;
    else if (command->kind == CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT)
      status = cnet_websocket_send_text(&peer->engine, command->data, command->size);
    else if (command->kind == CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY)
      status = cnet_websocket_send_binary(&peer->engine, command->data, command->size);
    else if (command->kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PING)
      status = cnet_websocket_send_ping(&peer->engine, command->data, command->size);
    else if (command->kind == CHTTP_SERVER_WEBSOCKET_COMMAND_PONG)
      status = cnet_websocket_send_pong(&peer->engine, command->data, command->size);
    else
      status = cnet_websocket_close(&peer->engine, command->close_code, command->data,
                                    command->size);
    if (status == SALTS_OK && command->session.stream_id != 0) {
      status = chttp_server_send_pending(connection);
      if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) status = SALTS_OK;
    }
    if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
    salts_mutex_lock(&server->mutex);
    free(command->data);
    *command = (chttp_server_websocket_command){0};
    server->websocket_command_head =
        (server->websocket_command_head + 1u) % server->config.network.command_capacity;
    --server->websocket_command_count;
    salts_mutex_unlock(&server->mutex);
  }
}
