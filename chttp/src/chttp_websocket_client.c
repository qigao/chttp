#include "chttp_h2_proto.h"
#include "chttp_tls.h"
#include "chttp_websocket_handshake.h"

#include <cnet/websocket.h>
#include <salts/clock.h>
#include <uri_parser.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_WEBSOCKET_CLIENT_DEFAULT_FRAME_BYTES = 64u * 1024u,
  CHTTP_WEBSOCKET_CLIENT_DEFAULT_HANDSHAKE_BYTES = 16u * 1024u,
  CHTTP_WEBSOCKET_CLIENT_H2_FRAME_HEADER_BYTES = 9u,
  CHTTP_WEBSOCKET_CLIENT_H2_MIN_FRAME_BYTES = 16u * 1024u,
  CHTTP_WEBSOCKET_CLIENT_H2_INPUT_BYTES = 128u * 1024u,
  CHTTP_WEBSOCKET_CLIENT_H2_HPACK_BYTES = 4096u,
  CHTTP_WEBSOCKET_CLIENT_H2_SETTINGS_COUNT = 16u,
  CHTTP_WEBSOCKET_CLIENT_H2_REQUIRED_HEADER_COUNT = 6u
};

typedef enum chttp_websocket_client_phase {
  CHTTP_WEBSOCKET_CLIENT_DISCONNECTED = 0,
  CHTTP_WEBSOCKET_CLIENT_CONNECTING,
  CHTTP_WEBSOCKET_CLIENT_HANDSHAKE,
  CHTTP_WEBSOCKET_CLIENT_OPEN,
  CHTTP_WEBSOCKET_CLIENT_TERMINAL
} chttp_websocket_client_phase;

typedef struct chttp_websocket_client_event_slot {
  chttp_websocket_event event;
  unsigned char *payload;
} chttp_websocket_client_event_slot;

typedef struct chttp_websocket_client_impl {
  cnet_client network;
  cnet_connection connection;
  cnet_websocket websocket;
  chttp_h2_proto *h2_protocol;
  chttp_tls_profile_impl *tls_profile;
  const char *expected_subprotocol;
  unsigned char *send_buffer;
  unsigned char *handshake_buffer;
  unsigned char *event_payloads;
  unsigned char *h2_frame_buffer;
  chttp_websocket_client_event_slot *events;
  size_t send_capacity;
  size_t handshake_capacity;
  size_t handshake_size;
  size_t event_capacity;
  size_t event_payload_capacity;
  size_t event_head;
  size_t event_count;
  size_t h2_frame_capacity;
  size_t h2_frame_size;
  size_t h2_wire_size;
  size_t max_frame_bytes;
  size_t max_message_bytes;
  size_t max_buffered_input_bytes;
  size_t h2_input_buffer_bytes;
  size_t h2_hpack_dynamic_table_bytes;
  size_t h2_max_settings_count;
  size_t expected_write_size;
  unsigned int handshake_http_status;
  int32_t h2_stream_id;
  int terminal_status;
  chttp_websocket_client_phase phase;
  chttp_protocol protocol;
  bool connected;
  bool receive_pending;
  bool write_pending;
  bool transport_terminal;
  bool event_overflow;
  bool operation_active;
  bool h2_header_block_open;
  bool h2_regular_header_seen;
  bool h2_status_seen;
  bool h2_subprotocol_seen;
  bool h2_stream_terminal;
  bool h2_end_submitted;
  bool h2_close_requested;
  char expected_accept[CHTTP_WEBSOCKET_ACCEPT_CAPACITY];
} chttp_websocket_client_impl;

static bool chttp_websocket_client_connection_matches(const chttp_websocket_client_impl *client,
                                                      cnet_connection connection) {
  return client != NULL && client->connection.slot == connection.slot &&
         client->connection.generation == connection.generation;
}

static void chttp_websocket_client_event_push(void *user, cnet_websocket *websocket,
                                              const cnet_websocket_event *event) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  chttp_websocket_client_event_slot *slot;
  size_t index;
  (void)websocket;
  if (client == NULL || event == NULL || client->event_overflow) return;
  if (client->event_count >= client->event_capacity ||
      event->size > client->event_payload_capacity) {
    client->event_overflow = true;
    client->terminal_status = SALTS_ENOBUFS;
    return;
  }
  index = (client->event_head + client->event_count) % client->event_capacity;
  slot = &client->events[index];
  if (event->size != 0u) memcpy(slot->payload, event->data, event->size);
  slot->event =
      (chttp_websocket_event){.kind = (chttp_websocket_event_kind)event->kind,
                              .message_type = (chttp_websocket_message_type)event->message_type,
                              .data = slot->payload,
                              .size = event->size,
                              .close_code = event->close_code};
  ++client->event_count;
}

static int chttp_websocket_client_write(void *user, const uint8_t *data, size_t size) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  int status;
  if (client == NULL || data == NULL || size == 0u || client->phase != CHTTP_WEBSOCKET_CLIENT_OPEN)
    return SALTS_EINVAL;
  if (client->protocol == CHTTP_HTTP_2) {
    if (client->h2_protocol == NULL || client->h2_stream_id == 0) return SALTS_EINVAL;
    if (client->h2_frame_size != 0u ||
        chttp_h2_proto_stream_output_pending(client->h2_protocol, client->h2_stream_id))
      return SALTS_EBUSY;
    if (size > client->h2_frame_capacity) return SALTS_EMSGSIZE;
    memcpy(client->h2_frame_buffer, data, size);
    client->h2_frame_size = size;
    if (chttp_h2_proto_submit_data(client->h2_protocol, client->h2_stream_id,
                                   client->h2_frame_buffer, size, 0) != 0) {
      client->h2_frame_size = 0u;
      return SALTS_ENOBUFS;
    }
    return SALTS_OK;
  }
  if (client->write_pending) return SALTS_EBUSY;
  status = cnet_send(&client->network, client->connection, data, size);
  if (status == SALTS_ENOBUFS) return SALTS_EBUSY;
  if (status != SALTS_OK) return status;
  client->write_pending = true;
  client->expected_write_size = size;
  return SALTS_OK;
}

static void chttp_websocket_client_on_send(void *user, cnet_connection connection, size_t size) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  if (!chttp_websocket_client_connection_matches(client, connection) || !client->write_pending ||
      size != client->expected_write_size) {
    if (client != NULL) client->terminal_status = SALTS_EPROTO;
    return;
  }
  client->write_pending = false;
  client->expected_write_size = 0u;
}

static void chttp_websocket_client_on_state(void *user, cnet_connection connection,
                                            cnet_connection_state state, const cnet_error *error) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  if (!chttp_websocket_client_connection_matches(client, connection)) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    client->connected = true;
    return;
  }
  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;
  client->transport_terminal = true;
  client->connected = false;
  client->receive_pending = false;
  client->write_pending = false;
  if (client->websocket.impl != NULL) (void)cnet_websocket_transport_closed(&client->websocket);
  if (client->terminal_status == SALTS_OK)
    client->terminal_status =
        error != NULL && error->status != SALTS_OK ? error->status : SALTS_ECONNRESET;
  client->phase = CHTTP_WEBSOCKET_CLIENT_TERMINAL;
}

static const unsigned char *chttp_websocket_client_header_end(const unsigned char *data,
                                                              size_t size) {
  size_t index;
  if (data == NULL || size < 4u) return NULL;
  for (index = 3u; index < size; ++index)
    if (data[index - 3u] == '\r' && data[index - 2u] == '\n' && data[index - 1u] == '\r' &&
        data[index] == '\n')
      return data + index + 1u;
  return NULL;
}

static int chttp_websocket_client_engine_init(chttp_websocket_client_impl *client) {
  const cnet_websocket_config config = {.size = sizeof(config),
                                        .role = CNET_WEBSOCKET_CLIENT,
                                        .max_frame_bytes = client->max_frame_bytes,
                                        .max_message_bytes = client->max_message_bytes,
                                        .max_buffered_input_bytes =
                                            client->max_buffered_input_bytes,
                                        .write = chttp_websocket_client_write,
                                        .on_event = chttp_websocket_client_event_push,
                                        .user = client};
  return cnet_websocket_init(&client->websocket, &config);
}

static bool chttp_websocket_client_h2_name(const char *name, size_t name_size,
                                           const char *expected) {
  const size_t expected_size = strlen(expected);
  return name != NULL && name_size == expected_size && memcmp(name, expected, name_size) == 0;
}

static int chttp_websocket_client_h2_begin_headers(void *user, int32_t stream_id) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  if (client == NULL || stream_id != client->h2_stream_id || client->h2_header_block_open ||
      client->phase != CHTTP_WEBSOCKET_CLIENT_HANDSHAKE)
    return -1;
  client->h2_header_block_open = true;
  client->h2_regular_header_seen = false;
  client->h2_status_seen = false;
  client->h2_subprotocol_seen = false;
  client->handshake_http_status = 0u;
  return 0;
}

static int chttp_websocket_client_h2_header(void *user, int32_t stream_id, const char *name,
                                            size_t name_size, const char *value,
                                            size_t value_size) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  size_t index;
  if (client == NULL || stream_id != client->h2_stream_id || !client->h2_header_block_open ||
      name == NULL || name_size == 0u || value == NULL)
    return -1;
  for (index = 0u; index < name_size; ++index)
    if (name[index] >= 'A' && name[index] <= 'Z') return -1;
  if (name[0] == ':') {
    if (client->h2_regular_header_seen || client->h2_status_seen ||
        !chttp_websocket_client_h2_name(name, name_size, ":status") || value_size != 3u ||
        value[0] < '1' || value[0] > '9' || value[1] < '0' || value[1] > '9' || value[2] < '0' ||
        value[2] > '9')
      return -1;
    client->handshake_http_status = (unsigned int)(value[0] - '0') * 100u +
                                    (unsigned int)(value[1] - '0') * 10u +
                                    (unsigned int)(value[2] - '0');
    client->h2_status_seen = true;
    return 0;
  }
  client->h2_regular_header_seen = true;
  if (chttp_websocket_client_h2_name(name, name_size, "sec-websocket-protocol")) {
    if (client->h2_subprotocol_seen || client->expected_subprotocol == NULL ||
        value_size != strlen(client->expected_subprotocol) ||
        memcmp(value, client->expected_subprotocol, value_size) != 0)
      return -1;
    client->h2_subprotocol_seen = true;
    return 0;
  }
  if (chttp_websocket_client_h2_name(name, name_size, "connection") ||
      chttp_websocket_client_h2_name(name, name_size, "proxy-connection") ||
      chttp_websocket_client_h2_name(name, name_size, "keep-alive") ||
      chttp_websocket_client_h2_name(name, name_size, "content-length") ||
      chttp_websocket_client_h2_name(name, name_size, "transfer-encoding") ||
      chttp_websocket_client_h2_name(name, name_size, "te") ||
      chttp_websocket_client_h2_name(name, name_size, "upgrade") ||
      chttp_websocket_client_h2_name(name, name_size, "sec-websocket-accept") ||
      chttp_websocket_client_h2_name(name, name_size, "sec-websocket-extensions"))
    return -1;
  return 0;
}

static int chttp_websocket_client_h2_end_headers(void *user, int32_t stream_id, int end_stream) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  int status;
  if (client == NULL || stream_id != client->h2_stream_id || !client->h2_header_block_open ||
      !client->h2_status_seen)
    return -1;
  client->h2_header_block_open = false;
  if (client->handshake_http_status != 200u) {
    client->terminal_status = SALTS_EPROTO;
    client->phase = CHTTP_WEBSOCKET_CLIENT_TERMINAL;
    return 0;
  }
  if ((client->expected_subprotocol == NULL && client->h2_subprotocol_seen) ||
      (client->expected_subprotocol != NULL && !client->h2_subprotocol_seen))
    return -1;
  if (end_stream) return -1;
  status = chttp_websocket_client_engine_init(client);
  if (status != SALTS_OK) {
    client->terminal_status = status;
    return -1;
  }
  client->phase = CHTTP_WEBSOCKET_CLIENT_OPEN;
  return 0;
}

static int chttp_websocket_client_h2_data(void *user, int32_t stream_id, const uint8_t *data,
                                          size_t size) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  int status = SALTS_OK;
  if (client == NULL || stream_id != client->h2_stream_id || (size != 0u && data == NULL))
    return -1;
  if (size != 0u && client->phase == CHTTP_WEBSOCKET_CLIENT_OPEN && client->websocket.impl != NULL)
    status = cnet_websocket_feed(&client->websocket, data, size);
  if (size != 0u && (chttp_h2_proto_consume_stream(client->h2_protocol, stream_id, size) != 0 ||
                     chttp_h2_proto_consume_connection(client->h2_protocol, size) != 0))
    return -1;
  if (status != SALTS_OK) {
    client->terminal_status = status;
    return -1;
  }
  if (chttp_h2_proto_remote_end_stream(client->h2_protocol, stream_id)) {
    if (client->websocket.impl != NULL) (void)cnet_websocket_transport_closed(&client->websocket);
    if (!client->h2_close_requested && client->terminal_status == SALTS_OK)
      client->terminal_status = SALTS_ECONNRESET;
  }
  return 0;
}

static int chttp_websocket_client_h2_stream_close(void *user, int32_t stream_id,
                                                  uint32_t error_code) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  if (client == NULL || stream_id != client->h2_stream_id) return 0;
  client->h2_stream_terminal = true;
  if (client->websocket.impl != NULL) (void)cnet_websocket_transport_closed(&client->websocket);
  if (error_code != CHTTP_H2_ERR_NO_ERROR && client->terminal_status == SALTS_OK)
    client->terminal_status = SALTS_ECONNRESET;
  return 0;
}

static void chttp_websocket_client_h2_goaway(void *user, uint32_t last_stream_id,
                                             uint32_t error_code) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  if (client == NULL || client->h2_stream_id == 0 ||
      (uint32_t)client->h2_stream_id <= last_stream_id)
    return;
  client->h2_stream_terminal = true;
  if (client->terminal_status == SALTS_OK)
    client->terminal_status = error_code == CHTTP_H2_ERR_NO_ERROR ? SALTS_ECONNRESET : SALTS_EPROTO;
}

static void chttp_websocket_client_on_receive(void *user, cnet_connection connection,
                                              const cnet_receive_view *view) {
  chttp_websocket_client_impl *client = (chttp_websocket_client_impl *)user;
  int status = SALTS_OK;
  if (!chttp_websocket_client_connection_matches(client, connection) || view == NULL ||
      view->kind != CNET_MESSAGE_BYTES) {
    if (client != NULL) client->terminal_status = SALTS_EPROTO;
    return;
  }
  client->receive_pending = false;
  if (client->protocol == CHTTP_HTTP_2) {
    const ptrdiff_t consumed = chttp_h2_proto_recv(client->h2_protocol, view->data, view->size);
    if (consumed < 0 || (size_t)consumed != view->size) status = SALTS_EPROTO;
  } else if (client->phase == CHTTP_WEBSOCKET_CLIENT_HANDSHAKE) {
    const unsigned char *header_end;
    size_t header_size;
    if (client->handshake_size > client->handshake_capacity ||
        view->size > client->handshake_capacity - client->handshake_size) {
      client->terminal_status = SALTS_EMSGSIZE;
      return;
    }
    if (view->size != 0u)
      memcpy(client->handshake_buffer + client->handshake_size, view->data, view->size);
    client->handshake_size += view->size;
    header_end =
        chttp_websocket_client_header_end(client->handshake_buffer, client->handshake_size);
    if (header_end == NULL) return;
    header_size = (size_t)(header_end - client->handshake_buffer);
    status = chttp_websocket_client_handshake_validate(client->handshake_buffer, header_size,
                                                       client->expected_accept,
                                                       client->expected_subprotocol,
                                                       &client->handshake_http_status);
    if (status == SALTS_OK) status = chttp_websocket_client_engine_init(client);
    if (status == SALTS_OK) client->phase = CHTTP_WEBSOCKET_CLIENT_OPEN;
    if (status == SALTS_OK && header_size < client->handshake_size)
      status = cnet_websocket_feed(&client->websocket, client->handshake_buffer + header_size,
                                   client->handshake_size - header_size);
    client->handshake_size = 0u;
  } else if (client->phase == CHTTP_WEBSOCKET_CLIENT_OPEN && client->websocket.impl != NULL) {
    status = cnet_websocket_feed(&client->websocket, view->data, view->size);
  } else {
    status = SALTS_EPROTO;
  }
  if (client->event_overflow) status = SALTS_ENOBUFS;
  if (status != SALTS_OK) client->terminal_status = status;
}

static uint64_t chttp_websocket_client_deadline(uint32_t timeout_ms) {
  const uint64_t now = salts_monotonic_ms();
  if (timeout_ms == 0u) return 0u;
  return UINT64_MAX - now < timeout_ms ? UINT64_MAX : now + timeout_ms;
}

static int chttp_websocket_client_poll(chttp_websocket_client_impl *client, uint64_t deadline) {
  uint32_t wait_ms = 1000u;
  size_t events = 0u;
  if (deadline != 0u) {
    const uint64_t now = salts_monotonic_ms();
    uint64_t remaining;
    if (now >= deadline) return SALTS_ETIMEDOUT;
    remaining = deadline - now;
    wait_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
  }
  return cnet_client_poll(&client->network, wait_ms, &events);
}

static int chttp_websocket_client_receive_arm(chttp_websocket_client_impl *client) {
  int status;
  if (client->receive_pending) return SALTS_OK;
  status = cnet_receive(&client->network, client->connection, 1u);
  if (status == SALTS_OK) client->receive_pending = true;
  return status;
}

static int chttp_websocket_client_h2_wire_flush(chttp_websocket_client_impl *client) {
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  int status;
  if (client == NULL || client->h2_protocol == NULL) return SALTS_EINVAL;
  if (client->h2_frame_size != 0u &&
      !chttp_h2_proto_stream_output_pending(client->h2_protocol, client->h2_stream_id))
    client->h2_frame_size = 0u;
  if (client->h2_close_requested && !client->h2_end_submitted && client->h2_frame_size == 0u &&
      client->websocket.impl != NULL && !cnet_websocket_has_pending_output(&client->websocket)) {
    cnet_websocket_state websocket_state = CNET_WEBSOCKET_OPEN;
    status = cnet_websocket_state_get(&client->websocket, &websocket_state);
    if (status != SALTS_OK) return status;
    if (websocket_state == CNET_WEBSOCKET_CLOSED || websocket_state == CNET_WEBSOCKET_FAILED) {
      if (chttp_h2_proto_submit_data(client->h2_protocol, client->h2_stream_id, NULL, 0u, 1) != 0)
        return SALTS_ENOBUFS;
      client->h2_end_submitted = true;
    }
  }
  if (client->write_pending) return SALTS_OK;
  if (client->h2_wire_size == 0u) {
    wire_size = chttp_h2_proto_send(client->h2_protocol, &wire);
    if (wire_size < 0 || (size_t)wire_size > client->send_capacity) return SALTS_EPROTO;
    if (wire_size == 0) return SALTS_OK;
    memcpy(client->send_buffer, wire, (size_t)wire_size);
    client->h2_wire_size = (size_t)wire_size;
  }
  status =
      cnet_send(&client->network, client->connection, client->send_buffer, client->h2_wire_size);
  if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
  if (status != SALTS_OK) return status;
  client->write_pending = true;
  client->expected_write_size = client->h2_wire_size;
  client->h2_wire_size = 0u;
  return SALTS_OK;
}

static bool chttp_websocket_client_h2_output_pending(chttp_websocket_client_impl *client) {
  return client->write_pending || client->h2_frame_size != 0u || client->h2_wire_size != 0u ||
         chttp_h2_proto_want_write(client->h2_protocol) ||
         (client->websocket.impl != NULL && cnet_websocket_has_pending_output(&client->websocket));
}

static int chttp_websocket_client_drain_output(chttp_websocket_client_impl *client,
                                               uint64_t deadline) {
  int status = SALTS_OK;
  if (client->protocol == CHTTP_HTTP_2) {
    do {
      if (client->h2_frame_size == 0u && client->websocket.impl != NULL &&
          cnet_websocket_has_pending_output(&client->websocket)) {
        status = cnet_websocket_flush(&client->websocket);
        if (status != SALTS_OK && status != SALTS_EBUSY) return status;
      }
      status = chttp_websocket_client_h2_wire_flush(client);
      if (status != SALTS_OK) return status;
      if (!chttp_websocket_client_h2_output_pending(client)) break;
      status = chttp_websocket_client_poll(client, deadline);
      if (status != SALTS_OK) return status;
    } while (!client->transport_terminal && client->terminal_status == SALTS_OK);
    return client->terminal_status == SALTS_OK ? status : client->terminal_status;
  }
  while (!client->transport_terminal && client->terminal_status == SALTS_OK &&
         (client->write_pending || cnet_websocket_has_pending_output(&client->websocket))) {
    if (!client->write_pending) {
      status = cnet_websocket_flush(&client->websocket);
      if (status != SALTS_OK && status != SALTS_EBUSY) return status;
      status = SALTS_OK;
    }
    if (client->write_pending || cnet_websocket_has_pending_output(&client->websocket)) {
      status = chttp_websocket_client_poll(client, deadline);
      if (status != SALTS_OK) return status;
    }
  }
  return client->terminal_status == SALTS_OK ? status : client->terminal_status;
}

static bool chttp_websocket_client_ascii_equal(const char *left, const char *right) {
  unsigned char a;
  unsigned char b;
  if (left == NULL || right == NULL) return false;
  do {
    a = (unsigned char)*left++;
    b = (unsigned char)*right++;
    if (a >= (unsigned char)'A' && a <= (unsigned char)'Z') a += 32u;
    if (b >= (unsigned char)'A' && b <= (unsigned char)'Z') b += 32u;
    if (a != b) return false;
  } while (a != 0u);
  return true;
}

static bool chttp_websocket_client_header_token(const char *name) {
  const unsigned char *cursor = (const unsigned char *)name;
  if (name == NULL || *name == '\0') return false;
  for (; *cursor != 0u; ++cursor) {
    if ((*cursor >= (unsigned char)'a' && *cursor <= (unsigned char)'z') ||
        (*cursor >= (unsigned char)'A' && *cursor <= (unsigned char)'Z') ||
        (*cursor >= (unsigned char)'0' && *cursor <= (unsigned char)'9') ||
        strchr("!#$%&'*+-.^_`|~", (int)*cursor) != NULL)
      continue;
    return false;
  }
  return true;
}

static bool chttp_websocket_client_header_value(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (value == NULL) return false;
  for (; *cursor != 0u; ++cursor)
    if (*cursor == '\r' || *cursor == '\n' || (*cursor < 0x20u && *cursor != '\t') ||
        *cursor == 0x7fu)
      return false;
  return true;
}

static bool chttp_websocket_client_header_owned(const char *name) {
  static const char *const owned[] = {"Host",
                                      "Upgrade",
                                      "Connection",
                                      "Sec-WebSocket-Key",
                                      "Sec-WebSocket-Version",
                                      "Sec-WebSocket-Accept",
                                      "Sec-WebSocket-Extensions",
                                      "Sec-WebSocket-Protocol",
                                      "Content-Length",
                                      "Transfer-Encoding"};
  size_t index;
  for (index = 0u; index < sizeof(owned) / sizeof(*owned); ++index)
    if (chttp_websocket_client_ascii_equal(name, owned[index])) return true;
  return false;
}

static int chttp_websocket_client_append(chttp_websocket_client_impl *client, size_t *size,
                                         const void *data, size_t data_size) {
  if (*size > client->send_capacity || data_size > client->send_capacity - *size)
    return SALTS_EMSGSIZE;
  if (data_size != 0u) memcpy(client->send_buffer + *size, data, data_size);
  *size += data_size;
  return SALTS_OK;
}

static int chttp_websocket_client_uri(const char *value, uri_t *uri, bool *secure, char *transport,
                                      size_t transport_capacity, char *authority,
                                      size_t authority_capacity, char *target,
                                      size_t target_capacity) {
  bool has_port;
  bool ipv6;
  const char *path;
  unsigned int port;
  int written;
  if (value == NULL || uri == NULL || secure == NULL || transport == NULL || authority == NULL ||
      target == NULL)
    return SALTS_EINVAL;
  if (uri_parse(value, uri) != 1) return uri->overflow_flags != 0u ? SALTS_ERANGE : SALTS_EINVAL;
  if (uri->overflow_flags != 0u) return SALTS_ERANGE;
  if (!uri->valid || uri->host[0] == '\0' ||
      (uri->component_flags & (URI_COMPONENT_USERINFO | URI_COMPONENT_FRAGMENT)) != 0u)
    return SALTS_EINVAL;
  has_port = (uri->component_flags & URI_COMPONENT_PORT) != 0u;
  ipv6 = uri->host_type == URI_HOST_IPV6ADDR;
  if (chttp_websocket_client_ascii_equal(uri->scheme, "wss")) *secure = true;
  else if (chttp_websocket_client_ascii_equal(uri->scheme, "ws")) *secure = false;
  else return SALTS_EPROTONOSUPPORT;
  if (has_port && (uri->port <= 0 || uri->port > UINT16_MAX)) return SALTS_ERANGE;
  port = has_port ? (unsigned int)uri->port : (*secure ? 443u : 80u);
  written = ipv6 ? snprintf(transport, transport_capacity, "%s://[%s]:%u", *secure ? "tls" : "tcp",
                            uri->host, port)
                 : snprintf(transport, transport_capacity, "%s://%s:%u", *secure ? "tls" : "tcp",
                            uri->host, port);
  if (written < 0 || (size_t)written >= transport_capacity) return SALTS_EMSGSIZE;
  written = ipv6 ? snprintf(authority, authority_capacity, "[%s]:%u", uri->host, port)
                 : snprintf(authority, authority_capacity, "%s:%u", uri->host, port);
  if (written < 0 || (size_t)written >= authority_capacity) return SALTS_EMSGSIZE;
  path = uri->path[0] == '\0' ? "/" : uri->path;
  written = (uri->component_flags & URI_COMPONENT_QUERY) != 0u
                ? snprintf(target, target_capacity, "%s?%s", path, uri->query)
                : snprintf(target, target_capacity, "%s", path);
  if (written < 0 || (size_t)written >= target_capacity) return SALTS_EMSGSIZE;
  return SALTS_OK;
}

static int chttp_websocket_client_request_build(chttp_websocket_client_impl *client,
                                                const chttp_websocket_connect_options *options,
                                                const char *authority, const char *target,
                                                const char *key, size_t *out_size) {
  static const char method[] = "GET ";
  static const char host_prefix[] = " HTTP/1.1\r\nHost: ";
  static const char upgrade[] =
      "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n";
  static const char key_prefix[] = "Sec-WebSocket-Key: ";
  static const char protocol_prefix[] = "Sec-WebSocket-Protocol: ";
  size_t size = 0u;
  size_t index;
  int status;
  status = chttp_websocket_client_append(client, &size, method, sizeof(method) - 1u);
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, target, strlen(target));
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, host_prefix, sizeof(host_prefix) - 1u);
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, authority, strlen(authority));
  if (status == SALTS_OK) status = chttp_websocket_client_append(client, &size, "\r\n", 2u);
  for (index = 0u; status == SALTS_OK && index < options->header_count; ++index) {
    const chttp_header *header = &options->headers[index];
    if (!chttp_websocket_client_header_token(header->name) ||
        !chttp_websocket_client_header_value(header->value) ||
        chttp_websocket_client_header_owned(header->name))
      return SALTS_EINVAL;
    status = chttp_websocket_client_append(client, &size, header->name, strlen(header->name));
    if (status == SALTS_OK) status = chttp_websocket_client_append(client, &size, ": ", 2u);
    if (status == SALTS_OK)
      status = chttp_websocket_client_append(client, &size, header->value, strlen(header->value));
    if (status == SALTS_OK) status = chttp_websocket_client_append(client, &size, "\r\n", 2u);
  }
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, upgrade, sizeof(upgrade) - 1u);
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, key_prefix, sizeof(key_prefix) - 1u);
  if (status == SALTS_OK)
    status = chttp_websocket_client_append(client, &size, key, CHTTP_WEBSOCKET_KEY_BYTES);
  if (status == SALTS_OK && options->subprotocol != NULL) {
    status = chttp_websocket_client_append(client, &size, "\r\n", 2u);
    if (status == SALTS_OK)
      status = chttp_websocket_client_append(client, &size, protocol_prefix,
                                             sizeof(protocol_prefix) - 1u);
    if (status == SALTS_OK)
      status = chttp_websocket_client_append(client, &size, options->subprotocol,
                                             strlen(options->subprotocol));
  }
  if (status == SALTS_OK) status = chttp_websocket_client_append(client, &size, "\r\n\r\n", 4u);
  if (status == SALTS_OK) *out_size = size;
  return status;
}

static int chttp_websocket_client_h2_init(chttp_websocket_client_impl *client) {
  const chttp_h2_proto_config config = {.stream_capacity = 1u,
                                        .output_buffer_bytes = client->send_capacity,
                                        .input_buffer_bytes = client->h2_input_buffer_bytes,
                                        .header_block_bytes = client->handshake_capacity,
                                        .max_header_list_bytes = client->handshake_capacity,
                                        .hpack_dynamic_table_bytes =
                                            client->h2_hpack_dynamic_table_bytes,
                                        .max_hpack_string_bytes = client->handshake_capacity,
                                        .max_settings_count = client->h2_max_settings_count};
  const chttp_h2_proto_callbacks callbacks = {
      .user_data = client,
      .on_begin_headers = chttp_websocket_client_h2_begin_headers,
      .on_header = chttp_websocket_client_h2_header,
      .on_end_headers = chttp_websocket_client_h2_end_headers,
      .on_data = chttp_websocket_client_h2_data,
      .on_stream_close = chttp_websocket_client_h2_stream_close,
      .on_goaway = chttp_websocket_client_h2_goaway};
  if (!chttp_h2_proto_config_valid(&config)) return SALTS_EMSGSIZE;
  client->h2_protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_CLIENT, &config, &callbacks);
  return client->h2_protocol == NULL ? SALTS_ENOMEM : SALTS_OK;
}

static int chttp_websocket_client_h2_submit(chttp_websocket_client_impl *client,
                                            const chttp_websocket_connect_options *options,
                                            const char *authority, const char *target,
                                            bool secure) {
  chttp_h2_hpack_header *headers;
  size_t header_count;
  size_t required_header_count;
  size_t header_bytes = 0u;
  size_t name_storage_used = 0u;
  size_t index;
  int status = SALTS_OK;
  required_header_count = CHTTP_WEBSOCKET_CLIENT_H2_REQUIRED_HEADER_COUNT +
                          (options->subprotocol != NULL ? 1u : 0u);
  if (options->header_count > SIZE_MAX - required_header_count) return SALTS_ERANGE;
  header_count = options->header_count + required_header_count;
  if (header_count > client->handshake_capacity / sizeof(*headers)) return SALTS_EMSGSIZE;
  headers = (chttp_h2_hpack_header *)calloc(header_count, sizeof(*headers));
  if (headers == NULL) return SALTS_ENOMEM;
  headers[0] =
      (chttp_h2_hpack_header){":method", sizeof(":method") - 1u, "CONNECT", sizeof("CONNECT") - 1u};
  headers[1] = (chttp_h2_hpack_header){":protocol", sizeof(":protocol") - 1u, "websocket",
                                       sizeof("websocket") - 1u};
  headers[2] = (chttp_h2_hpack_header){":scheme", sizeof(":scheme") - 1u, secure ? "https" : "http",
                                       secure ? 5u : 4u};
  headers[3] = (chttp_h2_hpack_header){":path", sizeof(":path") - 1u, target, strlen(target)};
  headers[4] = (chttp_h2_hpack_header){":authority", sizeof(":authority") - 1u, authority,
                                       strlen(authority)};
  headers[5] = (chttp_h2_hpack_header){"sec-websocket-version",
                                       sizeof("sec-websocket-version") - 1u, "13", 2u};
  if (options->subprotocol != NULL)
    headers[CHTTP_WEBSOCKET_CLIENT_H2_REQUIRED_HEADER_COUNT] =
        (chttp_h2_hpack_header){"sec-websocket-protocol",
                                sizeof("sec-websocket-protocol") - 1u, options->subprotocol,
                                strlen(options->subprotocol)};
  for (index = 0u; index < required_header_count; ++index) {
    size_t field_bytes;
    if (headers[index].name_size > SIZE_MAX - headers[index].value_size - 32u) {
      status = SALTS_EMSGSIZE;
      goto done;
    }
    field_bytes = headers[index].name_size + headers[index].value_size + 32u;
    if (field_bytes > client->handshake_capacity - header_bytes) {
      status = SALTS_EMSGSIZE;
      goto done;
    }
    header_bytes += field_bytes;
  }
  for (index = 0u; index < options->header_count; ++index) {
    const chttp_header *input = &options->headers[index];
    const size_t name_size = input->name == NULL ? 0u : strlen(input->name);
    const size_t value_size = input->value == NULL ? 0u : strlen(input->value);
    size_t byte_index;
    char *name;
    if (!chttp_websocket_client_header_token(input->name) ||
        !chttp_websocket_client_header_value(input->value) ||
        chttp_websocket_client_header_owned(input->name) ||
        name_size > client->handshake_capacity - name_storage_used) {
      status = SALTS_EINVAL;
      goto done;
    }
    if (name_size > SIZE_MAX - value_size - 32u ||
        name_size + value_size + 32u > client->handshake_capacity - header_bytes) {
      status = SALTS_EMSGSIZE;
      goto done;
    }
    name = (char *)client->handshake_buffer + name_storage_used;
    for (byte_index = 0u; byte_index < name_size; ++byte_index) {
      const unsigned char value = (unsigned char)input->name[byte_index];
      name[byte_index] = (char)(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    }
    name_storage_used += name_size;
    headers[required_header_count + index] =
        (chttp_h2_hpack_header){name, name_size, input->value, value_size};
    header_bytes += name_size + value_size + 32u;
  }
  if (chttp_h2_proto_submit_request_headers(client->h2_protocol, headers, header_count, 0,
                                            &client->h2_stream_id) != 0)
    status = SALTS_EPROTO;

done:
  free(headers);
  return status;
}

int chttp_websocket_client_init(chttp_websocket_client *client,
                                const chttp_websocket_client_config *config) {
  chttp_websocket_client_impl *impl;
  size_t frame_bytes;
  size_t message_bytes;
  size_t input_bytes;
  size_t output_bytes;
  size_t event_capacity;
  size_t event_payload_capacity;
  size_t event_payload_bytes;
  int status;
  if (client == NULL || config == NULL || client->impl != NULL || config->size != sizeof(*config) ||
      config->network.max_send_bytes <= CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EINVAL;
  if (config->socket_options.size != 0u) {
    status = cnet_stream_socket_options_validate(&config->socket_options);
    if (status != SALTS_OK) return status;
  }
  frame_bytes = config->max_frame_bytes;
  if (frame_bytes == 0u) {
    frame_bytes = config->network.max_send_bytes - CNET_WEBSOCKET_MAX_HEADER_BYTES;
    if (frame_bytes > CHTTP_WEBSOCKET_CLIENT_DEFAULT_FRAME_BYTES)
      frame_bytes = CHTTP_WEBSOCKET_CLIENT_DEFAULT_FRAME_BYTES;
  }
  message_bytes = config->max_message_bytes == 0u ? frame_bytes : config->max_message_bytes;
  if (frame_bytes < CNET_WEBSOCKET_MIN_FRAME_BYTES || message_bytes < frame_bytes ||
      frame_bytes > SIZE_MAX - CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EINVAL;
  output_bytes = frame_bytes + CNET_WEBSOCKET_MAX_HEADER_BYTES;
  input_bytes =
      config->max_buffered_input_bytes == 0u ? output_bytes : config->max_buffered_input_bytes;
  if (output_bytes > config->network.max_send_bytes || input_bytes < output_bytes)
    return SALTS_EMSGSIZE;
  if (input_bytes > SIZE_MAX - message_bytes ||
      input_bytes + message_bytes > SIZE_MAX - output_bytes)
    return SALTS_ERANGE;
  event_capacity =
      config->event_capacity == 0u ? config->network.event_capacity : config->event_capacity;
  event_payload_capacity = message_bytes > CNET_WEBSOCKET_MAX_CONTROL_BYTES
                               ? message_bytes
                               : CNET_WEBSOCKET_MAX_CONTROL_BYTES;
  if (event_capacity == 0u || event_capacity > SIZE_MAX / sizeof(*impl->events) ||
      event_payload_capacity > SIZE_MAX / event_capacity)
    return SALTS_ERANGE;
  event_payload_bytes = event_payload_capacity * event_capacity;
  impl = (chttp_websocket_client_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->send_capacity = config->network.max_send_bytes;
  impl->handshake_capacity = config->max_handshake_header_bytes == 0u
                                 ? CHTTP_WEBSOCKET_CLIENT_DEFAULT_HANDSHAKE_BYTES
                                 : config->max_handshake_header_bytes;
  impl->event_capacity = event_capacity;
  impl->event_payload_capacity = event_payload_capacity;
  impl->h2_frame_capacity = output_bytes;
  impl->max_frame_bytes = frame_bytes;
  impl->max_message_bytes = message_bytes;
  impl->max_buffered_input_bytes = input_bytes;
  impl->h2_input_buffer_bytes = config->h2_input_buffer_bytes == 0u
                                    ? CHTTP_WEBSOCKET_CLIENT_H2_INPUT_BYTES
                                    : config->h2_input_buffer_bytes;
  impl->h2_hpack_dynamic_table_bytes = config->h2_hpack_dynamic_table_bytes == 0u
                                           ? CHTTP_WEBSOCKET_CLIENT_H2_HPACK_BYTES
                                           : config->h2_hpack_dynamic_table_bytes;
  impl->h2_max_settings_count = config->h2_max_settings_count == 0u
                                    ? CHTTP_WEBSOCKET_CLIENT_H2_SETTINGS_COUNT
                                    : config->h2_max_settings_count;
  if (impl->h2_input_buffer_bytes < CHTTP_WEBSOCKET_CLIENT_H2_FRAME_HEADER_BYTES +
                                        CHTTP_WEBSOCKET_CLIENT_H2_MIN_FRAME_BYTES ||
      impl->h2_input_buffer_bytes > PTRDIFF_MAX ||
      impl->h2_hpack_dynamic_table_bytes > UINT32_MAX ||
      impl->h2_max_settings_count > SIZE_MAX / sizeof(uint32_t)) {
    status = SALTS_EMSGSIZE;
    goto fail;
  }
  impl->send_buffer = (unsigned char *)malloc(impl->send_capacity);
  impl->h2_frame_buffer = (unsigned char *)malloc(output_bytes);
  impl->handshake_buffer = (unsigned char *)malloc(impl->handshake_capacity);
  impl->events = (chttp_websocket_client_event_slot *)calloc(event_capacity, sizeof(*impl->events));
  impl->event_payloads = (unsigned char *)malloc(event_payload_bytes);
  if (impl->send_buffer == NULL || impl->h2_frame_buffer == NULL ||
      impl->handshake_buffer == NULL || impl->events == NULL || impl->event_payloads == NULL) {
    status = SALTS_ENOMEM;
    goto fail;
  }
  for (size_t index = 0u; index < event_capacity; ++index)
    impl->events[index].payload = impl->event_payloads + index * event_payload_capacity;
  status = cnet_client_init(&impl->network, &config->network);
  if (status != SALTS_OK) goto fail;
  if (config->socket_options.size != 0u) {
    status = cnet_client_set_stream_socket_options(&impl->network, &config->socket_options);
    if (status != SALTS_OK) goto fail;
  }
  client->impl = impl;
  return SALTS_OK;

fail:
  if (impl->network.impl != NULL) {
    (void)cnet_client_stop(&impl->network, 0u);
    (void)cnet_client_destroy(&impl->network);
  }
  free(impl->h2_frame_buffer);
  free(impl->event_payloads);
  free(impl->events);
  free(impl->handshake_buffer);
  free(impl->send_buffer);
  free(impl);
  return status;
}

int chttp_websocket_client_connect(chttp_websocket_client *client,
                                   const chttp_websocket_connect_options *options,
                                   unsigned int *out_http_status) {
  chttp_websocket_client_impl *impl;
  uri_t uri;
  char transport[320];
  char authority[320];
  char target[2050];
  char key[CHTTP_WEBSOCKET_KEY_CAPACITY];
  bool secure = false;
  size_t request_size = 0u;
  uint64_t deadline;
  cnet_observer observer;
  cnet_connect_options connect_options;
  int status;
  if (out_http_status != NULL) *out_http_status = 0u;
  if (client == NULL || client->impl == NULL || options == NULL ||
      options->size != sizeof(*options) || options->uri == NULL || out_http_status == NULL ||
      (options->header_count != 0u && options->headers == NULL) ||
      (options->protocol != CHTTP_HTTP_1_1 && options->protocol != CHTTP_HTTP_2) ||
      (options->subprotocol != NULL &&
       !chttp_websocket_client_header_token(options->subprotocol)))
    return SALTS_EINVAL;
  impl = (chttp_websocket_client_impl *)client->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->phase != CHTTP_WEBSOCKET_CLIENT_DISCONNECTED) return SALTS_EALREADY;
  impl->operation_active = true;
  impl->protocol = options->protocol;
  impl->expected_subprotocol = options->subprotocol;
  status = chttp_websocket_client_uri(options->uri, &uri, &secure, transport, sizeof(transport),
                                      authority, sizeof(authority), target, sizeof(target));
  if (status != SALTS_OK) goto done;
  if ((!secure && options->tls != NULL) ||
      (secure && options->tls != NULL && options->tls->impl == NULL)) {
    status = SALTS_EINVAL;
    goto done;
  }
  if (secure) {
    status = chttp_tls_profile_acquire(options->tls, &impl->tls_profile);
    if (status != SALTS_OK) goto done;
    if ((options->protocol == CHTTP_HTTP_2 && impl->tls_profile == NULL) ||
        (impl->tls_profile != NULL &&
         chttp_tls_profile_protocol(impl->tls_profile) != options->protocol)) {
      status = SALTS_EPROTONOSUPPORT;
      goto done;
    }
  }
  if (options->protocol == CHTTP_HTTP_2) {
    status = chttp_websocket_client_h2_init(impl);
  } else {
    status = chttp_websocket_client_key_generate(key, sizeof(key));
    if (status == SALTS_OK)
      status =
          chttp_websocket_accept_compute(key, impl->expected_accept, sizeof(impl->expected_accept));
    if (status == SALTS_OK)
      status = chttp_websocket_client_request_build(impl, options, authority, target, key,
                                                    &request_size);
  }
  if (status != SALTS_OK) goto done;
  observer = (cnet_observer){.on_state = chttp_websocket_client_on_state,
                             .on_receive = chttp_websocket_client_on_receive,
                             .on_send = chttp_websocket_client_on_send,
                             .user = impl};
  connect_options =
      (cnet_connect_options){.uri = transport,
                             .observer = observer,
                             .tls_client = chttp_tls_profile_client(impl->tls_profile)};
  impl->phase = CHTTP_WEBSOCKET_CLIENT_CONNECTING;
  status = cnet_connect(&impl->network, &connect_options, &impl->connection);
  if (status != SALTS_OK) goto done;
  deadline = chttp_websocket_client_deadline(options->timeout_ms);
  while (!impl->connected && !impl->transport_terminal && impl->terminal_status == SALTS_OK) {
    status = chttp_websocket_client_poll(impl, deadline);
    if (status != SALTS_OK) goto done;
  }
  if (!impl->connected) {
    status = impl->terminal_status == SALTS_OK ? SALTS_ECONNRESET : impl->terminal_status;
    goto done;
  }
  impl->phase = CHTTP_WEBSOCKET_CLIENT_HANDSHAKE;
  if (options->protocol == CHTTP_HTTP_2) {
    status = chttp_websocket_client_h2_wire_flush(impl);
    if (status != SALTS_OK) goto done;
    while (!chttp_h2_proto_peer_settings_received(impl->h2_protocol) && !impl->transport_terminal &&
           impl->terminal_status == SALTS_OK) {
      status = chttp_websocket_client_receive_arm(impl);
      if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) status = SALTS_OK;
      if (status != SALTS_OK) goto done;
      status = chttp_websocket_client_poll(impl, deadline);
      if (status != SALTS_OK) goto done;
      status = chttp_websocket_client_h2_wire_flush(impl);
      if (status != SALTS_OK) goto done;
    }
    if (!chttp_h2_proto_peer_settings_received(impl->h2_protocol) ||
        chttp_h2_proto_peer_enable_connect_protocol(impl->h2_protocol) != 1u) {
      status = SALTS_EPROTONOSUPPORT;
      goto done;
    }
    status = chttp_websocket_client_h2_submit(impl, options, authority, target, secure);
    if (status != SALTS_OK) goto done;
    status = chttp_websocket_client_drain_output(impl, deadline);
    if (status != SALTS_OK) goto done;
  } else {
    status = cnet_send(&impl->network, impl->connection, impl->send_buffer, request_size);
    if (status != SALTS_OK) goto done;
    impl->write_pending = true;
    impl->expected_write_size = request_size;
    while (impl->write_pending && !impl->transport_terminal && impl->terminal_status == SALTS_OK) {
      status = chttp_websocket_client_poll(impl, deadline);
      if (status != SALTS_OK) goto done;
    }
  }
  while (impl->phase == CHTTP_WEBSOCKET_CLIENT_HANDSHAKE && !impl->transport_terminal &&
         impl->terminal_status == SALTS_OK) {
    status = chttp_websocket_client_receive_arm(impl);
    if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) {
      status = chttp_websocket_client_poll(impl, deadline);
      if (status != SALTS_OK) goto done;
      continue;
    }
    if (status != SALTS_OK) goto done;
    status = chttp_websocket_client_poll(impl, deadline);
    if (status != SALTS_OK) goto done;
    if (options->protocol == CHTTP_HTTP_2) {
      status = chttp_websocket_client_h2_wire_flush(impl);
      if (status != SALTS_OK) goto done;
    }
  }
  if (impl->phase == CHTTP_WEBSOCKET_CLIENT_OPEN) {
    status = chttp_websocket_client_drain_output(impl, deadline);
    if (status != SALTS_OK) goto done;
  }
  *out_http_status = impl->handshake_http_status;
  status = impl->phase == CHTTP_WEBSOCKET_CLIENT_OPEN ? SALTS_OK : impl->terminal_status;

done:
  if (status != SALTS_OK && impl->phase != CHTTP_WEBSOCKET_CLIENT_OPEN) {
    if (impl->connection.generation != 0u && !impl->transport_terminal)
      (void)cnet_close(&impl->network, impl->connection);
    impl->phase = CHTTP_WEBSOCKET_CLIENT_TERMINAL;
  }
  if (status != SALTS_OK && impl->tls_profile != NULL) {
    chttp_tls_profile_release(impl->tls_profile);
    impl->tls_profile = NULL;
  }
  impl->expected_subprotocol = NULL;
  impl->operation_active = false;
  return status;
}

typedef int (*chttp_websocket_client_send_fn)(cnet_websocket *websocket, const void *data,
                                              size_t size);

static int chttp_websocket_client_send(chttp_websocket_client *client,
                                       chttp_websocket_client_send_fn send, const void *data,
                                       size_t size, uint32_t timeout_ms) {
  chttp_websocket_client_impl *impl;
  uint64_t deadline;
  int status;
  if (client == NULL || client->impl == NULL || send == NULL || (data == NULL && size != 0u))
    return SALTS_EINVAL;
  impl = (chttp_websocket_client_impl *)client->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->phase != CHTTP_WEBSOCKET_CLIENT_OPEN || impl->terminal_status != SALTS_OK)
    return SALTS_ESHUTDOWN;
  impl->operation_active = true;
  deadline = chttp_websocket_client_deadline(timeout_ms);
  status = chttp_websocket_client_drain_output(impl, deadline);
  if (status == SALTS_OK) status = send(&impl->websocket, data, size);
  if (status == SALTS_OK) status = chttp_websocket_client_drain_output(impl, deadline);
  if (status == SALTS_OK && impl->terminal_status != SALTS_OK) status = impl->terminal_status;
  impl->operation_active = false;
  return status;
}

int chttp_websocket_client_send_text(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms) {
  return chttp_websocket_client_send(client, cnet_websocket_send_text, data, size, timeout_ms);
}

int chttp_websocket_client_send_binary(chttp_websocket_client *client, const void *data,
                                       size_t size, uint32_t timeout_ms) {
  return chttp_websocket_client_send(client, cnet_websocket_send_binary, data, size, timeout_ms);
}

int chttp_websocket_client_send_ping(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms) {
  return chttp_websocket_client_send(client, cnet_websocket_send_ping, data, size, timeout_ms);
}

int chttp_websocket_client_send_pong(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms) {
  return chttp_websocket_client_send(client, cnet_websocket_send_pong, data, size, timeout_ms);
}

int chttp_websocket_client_receive(chttp_websocket_client *client, uint32_t timeout_ms,
                                   chttp_websocket_event *out_event) {
  chttp_websocket_client_impl *impl;
  uint64_t deadline;
  int status = SALTS_OK;
  if (client == NULL || client->impl == NULL || out_event == NULL) return SALTS_EINVAL;
  *out_event = (chttp_websocket_event){0};
  impl = (chttp_websocket_client_impl *)client->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->phase != CHTTP_WEBSOCKET_CLIENT_OPEN && impl->event_count == 0u) return SALTS_ESHUTDOWN;
  impl->operation_active = true;
  deadline = chttp_websocket_client_deadline(timeout_ms);
  while (impl->event_count == 0u && !impl->transport_terminal &&
         impl->terminal_status == SALTS_OK) {
    status = chttp_websocket_client_receive_arm(impl);
    if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) status = SALTS_OK;
    if (status != SALTS_OK) break;
    status = chttp_websocket_client_poll(impl, deadline);
    if (status != SALTS_OK) break;
  }
  if (status == SALTS_OK && impl->event_count != 0u && !impl->transport_terminal &&
      impl->terminal_status == SALTS_OK)
    status = chttp_websocket_client_drain_output(impl, deadline);
  if (status == SALTS_OK && impl->event_count != 0u) {
    *out_event = impl->events[impl->event_head].event;
    impl->event_head = (impl->event_head + 1u) % impl->event_capacity;
    --impl->event_count;
  } else if (status == SALTS_OK) {
    status = impl->terminal_status == SALTS_OK ? SALTS_ECONNRESET : impl->terminal_status;
  }
  impl->operation_active = false;
  return status;
}

int chttp_websocket_client_close(chttp_websocket_client *client, uint16_t code, const void *reason,
                                 size_t reason_size, uint32_t timeout_ms) {
  chttp_websocket_client_impl *impl;
  uint64_t deadline;
  int status;
  if (client == NULL || client->impl == NULL || (reason == NULL && reason_size != 0u))
    return SALTS_EINVAL;
  impl = (chttp_websocket_client_impl *)client->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->phase != CHTTP_WEBSOCKET_CLIENT_OPEN) return SALTS_EALREADY;
  impl->operation_active = true;
  deadline = chttp_websocket_client_deadline(timeout_ms);
  status = chttp_websocket_client_drain_output(impl, deadline);
  if (impl->protocol == CHTTP_HTTP_2) impl->h2_close_requested = true;
  if (status == SALTS_OK)
    status = cnet_websocket_close(&impl->websocket, code, reason, reason_size);
  if (status != SALTS_OK) {
    impl->operation_active = false;
    return status;
  }
  if (impl->protocol == CHTTP_HTTP_2) {
    while (status == SALTS_OK && !impl->transport_terminal && !impl->h2_stream_terminal &&
           impl->terminal_status == SALTS_OK) {
      status = chttp_websocket_client_drain_output(impl, deadline);
      if (status != SALTS_OK || impl->h2_stream_terminal) break;
      status = chttp_websocket_client_receive_arm(impl);
      if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) status = SALTS_OK;
      if (status != SALTS_OK) break;
      status = chttp_websocket_client_poll(impl, deadline);
    }
  } else
    while (status == SALTS_OK && !impl->transport_terminal && impl->terminal_status == SALTS_OK) {
      cnet_websocket_state websocket_state = CNET_WEBSOCKET_OPEN;
      if (!impl->write_pending && cnet_websocket_has_pending_output(&impl->websocket)) {
        status = cnet_websocket_flush(&impl->websocket);
        if (status == SALTS_EBUSY) status = SALTS_OK;
        if (status != SALTS_OK) break;
      }
      if (!impl->write_pending) {
        status = cnet_websocket_state_get(&impl->websocket, &websocket_state);
        if (status != SALTS_OK || websocket_state == CNET_WEBSOCKET_CLOSED ||
            websocket_state == CNET_WEBSOCKET_FAILED)
          break;
        status = chttp_websocket_client_receive_arm(impl);
        if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) status = SALTS_OK;
        if (status != SALTS_OK) break;
      }
      status = chttp_websocket_client_poll(impl, deadline);
    }
  if (!impl->transport_terminal) {
    const int close_status = cnet_close(&impl->network, impl->connection);
    if (status == SALTS_OK && close_status != SALTS_OK && close_status != SALTS_EALREADY)
      status = close_status;
  }
  impl->phase = CHTTP_WEBSOCKET_CLIENT_TERMINAL;
  impl->operation_active = false;
  return status;
}

int chttp_websocket_client_destroy(chttp_websocket_client *client, uint32_t timeout_ms) {
  chttp_websocket_client_impl *impl;
  int first_status = SALTS_OK;
  int status;
  if (client == NULL) return SALTS_EINVAL;
  impl = (chttp_websocket_client_impl *)client->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->operation_active) return SALTS_EBUSY;
  if (!impl->transport_terminal && impl->connection.generation != 0u) {
    status = cnet_close(&impl->network, impl->connection);
    if (status != SALTS_OK && status != SALTS_EALREADY && status != SALTS_ESHUTDOWN)
      first_status = status;
  }
  status = cnet_client_stop(&impl->network, timeout_ms);
  if (status == SALTS_ETIMEDOUT) return status;
  if (first_status == SALTS_OK && status != SALTS_OK && status != SALTS_EALREADY)
    first_status = status;
  status = cnet_client_destroy(&impl->network);
  if (first_status == SALTS_OK && status != SALTS_OK) first_status = status;
  if (status != SALTS_OK) return first_status;
  if (impl->websocket.impl != NULL) {
    status = cnet_websocket_destroy(&impl->websocket);
    if (first_status == SALTS_OK && status != SALTS_OK) first_status = status;
  }
  chttp_h2_proto_destroy(impl->h2_protocol);
  chttp_tls_profile_release(impl->tls_profile);
  free(impl->h2_frame_buffer);
  free(impl->event_payloads);
  free(impl->events);
  free(impl->handshake_buffer);
  free(impl->send_buffer);
  free(impl);
  client->impl = NULL;
  return first_status;
}
