#include "chttp_h2_proto.h"
#include "chttp_tls.h"

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
  CHTTP_WEBSOCKET_POOL_DEFAULT_FRAME_BYTES = 64u * 1024u,
  CHTTP_WEBSOCKET_POOL_DEFAULT_HANDSHAKE_BYTES = 16u * 1024u,
  CHTTP_WEBSOCKET_POOL_H2_FRAME_HEADER_BYTES = 9u,
  CHTTP_WEBSOCKET_POOL_H2_MIN_FRAME_BYTES = 16u * 1024u,
  CHTTP_WEBSOCKET_POOL_H2_INPUT_BYTES = 128u * 1024u,
  CHTTP_WEBSOCKET_POOL_H2_HPACK_BYTES = 4096u,
  CHTTP_WEBSOCKET_POOL_H2_SETTINGS_COUNT = 16u,
  CHTTP_WEBSOCKET_POOL_TRANSPORT_BYTES = 320u,
  CHTTP_WEBSOCKET_POOL_AUTHORITY_BYTES = 320u,
  CHTTP_WEBSOCKET_POOL_TARGET_BYTES = 2050u
};

typedef enum chttp_websocket_pool_phase {
  CHTTP_WEBSOCKET_POOL_DISCONNECTED = 0,
  CHTTP_WEBSOCKET_POOL_CONNECTING,
  CHTTP_WEBSOCKET_POOL_CONNECTED,
  CHTTP_WEBSOCKET_POOL_TERMINAL
} chttp_websocket_pool_phase;

typedef enum chttp_websocket_pool_slot_phase {
  CHTTP_WEBSOCKET_POOL_SLOT_FREE = 0,
  CHTTP_WEBSOCKET_POOL_SLOT_OPENING,
  CHTTP_WEBSOCKET_POOL_SLOT_OPEN,
  CHTTP_WEBSOCKET_POOL_SLOT_CLOSING,
  CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL
} chttp_websocket_pool_slot_phase;

typedef struct chttp_websocket_pool_event_slot {
  chttp_websocket_event event;
  unsigned char *payload;
} chttp_websocket_pool_event_slot;

typedef struct chttp_websocket_pool_impl chttp_websocket_pool_impl;

typedef struct chttp_websocket_pool_slot {
  chttp_websocket_pool_impl *pool;
  cnet_websocket websocket;
  chttp_websocket_pool_event_slot *events;
  unsigned char *event_payloads;
  unsigned char *frame_buffer;
  size_t event_head;
  size_t event_count;
  size_t frame_size;
  unsigned int http_status;
  int32_t stream_id;
  uint32_t generation;
  int terminal_status;
  chttp_websocket_pool_slot_phase phase;
  bool header_block_open;
  bool regular_header_seen;
  bool status_seen;
  bool stream_terminal;
  bool end_submitted;
  bool close_requested;
  bool event_overflow;
} chttp_websocket_pool_slot;

struct chttp_websocket_pool_impl {
  cnet_client network;
  cnet_connection connection;
  chttp_h2_proto *protocol;
  chttp_tls_profile_impl *tls_profile;
  chttp_websocket_pool_slot *slots;
  chttp_websocket_pool_event_slot *event_slots;
  unsigned char *event_payloads;
  unsigned char *frame_buffers;
  unsigned char *wire_buffer;
  unsigned char *header_name_buffer;
  size_t session_capacity;
  size_t active_sessions;
  size_t event_capacity;
  size_t event_payload_capacity;
  size_t frame_capacity;
  size_t wire_capacity;
  size_t wire_size;
  size_t header_capacity;
  size_t max_frame_bytes;
  size_t max_message_bytes;
  size_t max_buffered_input_bytes;
  size_t expected_write_size;
  int terminal_status;
  chttp_websocket_pool_phase phase;
  bool secure;
  bool receive_pending;
  bool write_pending;
  bool transport_terminal;
  bool operation_active;
  bool draining;
  char transport[CHTTP_WEBSOCKET_POOL_TRANSPORT_BYTES];
  char authority[CHTTP_WEBSOCKET_POOL_AUTHORITY_BYTES];
};

static bool chttp_websocket_pool_size_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
  *out = left * right;
  return true;
}

static uint32_t chttp_websocket_pool_next_generation(uint32_t generation) {
  return generation == UINT32_MAX ? 1u : generation + 1u;
}

static uint64_t chttp_websocket_pool_deadline(uint32_t timeout_ms) {
  const uint64_t now = salts_monotonic_ms();
  if (timeout_ms == 0u) return 0u;
  return UINT64_MAX - now < timeout_ms ? UINT64_MAX : now + timeout_ms;
}

static uint32_t chttp_websocket_pool_remaining(uint64_t deadline) {
  const uint64_t now = salts_monotonic_ms();
  const uint64_t remaining = deadline > now ? deadline - now : 0u;
  return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static bool chttp_websocket_pool_connection_matches(const chttp_websocket_pool_impl *pool,
                                                    cnet_connection connection) {
  return pool != NULL && pool->connection.slot == connection.slot &&
         pool->connection.generation == connection.generation;
}

static bool chttp_websocket_pool_ascii_equal(const char *left, const char *right) {
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

static bool chttp_websocket_pool_header_token(const char *name) {
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

static bool chttp_websocket_pool_header_value(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (value == NULL) return false;
  for (; *cursor != 0u; ++cursor)
    if (*cursor == '\r' || *cursor == '\n' || (*cursor < 0x20u && *cursor != '\t') ||
        *cursor == 0x7fu)
      return false;
  return true;
}

static bool chttp_websocket_pool_header_owned(const char *name) {
  static const char *const owned[] = {"Host",
                                      "Upgrade",
                                      "Connection",
                                      "Proxy-Connection",
                                      "Keep-Alive",
                                      "Sec-WebSocket-Key",
                                      "Sec-WebSocket-Version",
                                      "Sec-WebSocket-Accept",
                                      "Sec-WebSocket-Extensions",
                                      "Sec-WebSocket-Protocol",
                                      "Content-Length",
                                      "Transfer-Encoding",
                                      "TE"};
  size_t index;
  for (index = 0u; index < sizeof(owned) / sizeof(*owned); ++index)
    if (chttp_websocket_pool_ascii_equal(name, owned[index])) return true;
  return false;
}

static bool chttp_websocket_pool_h2_name(const char *name, size_t name_size, const char *expected) {
  const size_t expected_size = strlen(expected);
  return name != NULL && name_size == expected_size && memcmp(name, expected, name_size) == 0;
}

static int chttp_websocket_pool_uri(const char *value, bool *secure, char *transport,
                                    size_t transport_capacity, char *authority,
                                    size_t authority_capacity, char *target,
                                    size_t target_capacity) {
  uri_t uri;
  bool has_port;
  bool ipv6;
  const char *path;
  unsigned int port;
  int written;
  if (value == NULL || secure == NULL || transport == NULL || authority == NULL || target == NULL)
    return SALTS_EINVAL;
  if (uri_parse(value, &uri) != 1) return uri.overflow_flags != 0u ? SALTS_ERANGE : SALTS_EINVAL;
  if (uri.overflow_flags != 0u) return SALTS_ERANGE;
  if (!uri.valid || uri.host[0] == '\0' ||
      (uri.component_flags & (URI_COMPONENT_USERINFO | URI_COMPONENT_FRAGMENT)) != 0u)
    return SALTS_EINVAL;
  has_port = (uri.component_flags & URI_COMPONENT_PORT) != 0u;
  ipv6 = uri.host_type == URI_HOST_IPV6ADDR;
  if (chttp_websocket_pool_ascii_equal(uri.scheme, "wss")) *secure = true;
  else if (chttp_websocket_pool_ascii_equal(uri.scheme, "ws")) *secure = false;
  else return SALTS_EPROTONOSUPPORT;
  if (has_port && (uri.port <= 0 || uri.port > UINT16_MAX)) return SALTS_ERANGE;
  port = has_port ? (unsigned int)uri.port : (*secure ? 443u : 80u);
  written = ipv6 ? snprintf(transport, transport_capacity, "%s://[%s]:%u", *secure ? "tls" : "tcp",
                            uri.host, port)
                 : snprintf(transport, transport_capacity, "%s://%s:%u", *secure ? "tls" : "tcp",
                            uri.host, port);
  if (written < 0 || (size_t)written >= transport_capacity) return SALTS_EMSGSIZE;
  written = ipv6 ? snprintf(authority, authority_capacity, "[%s]:%u", uri.host, port)
                 : snprintf(authority, authority_capacity, "%s:%u", uri.host, port);
  if (written < 0 || (size_t)written >= authority_capacity) return SALTS_EMSGSIZE;
  path = uri.path[0] == '\0' ? "/" : uri.path;
  written = (uri.component_flags & URI_COMPONENT_QUERY) != 0u
                ? snprintf(target, target_capacity, "%s?%s", path, uri.query)
                : snprintf(target, target_capacity, "%s", path);
  return written < 0 || (size_t)written >= target_capacity ? SALTS_EMSGSIZE : SALTS_OK;
}

static chttp_websocket_pool_slot *chttp_websocket_pool_stream(chttp_websocket_pool_impl *pool,
                                                              int32_t stream_id) {
  size_t index;
  if (pool == NULL || stream_id <= 0) return NULL;
  for (index = 0u; index < pool->session_capacity; ++index)
    if (pool->slots[index].phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE &&
        pool->slots[index].stream_id == stream_id)
      return &pool->slots[index];
  return NULL;
}

static chttp_websocket_pool_slot *chttp_websocket_pool_session(chttp_websocket_pool_impl *pool,
                                                               chttp_websocket_session session) {
  chttp_websocket_pool_slot *slot;
  if (pool == NULL || session.slot == 0u || session.generation == 0u ||
      session.slot > pool->session_capacity)
    return NULL;
  slot = &pool->slots[session.slot - 1u];
  return slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE && slot->generation == session.generation
             ? slot
             : NULL;
}

static void chttp_websocket_pool_slot_transport_closed(chttp_websocket_pool_slot *slot) {
  if (slot != NULL && slot->websocket.impl != NULL)
    (void)cnet_websocket_transport_closed(&slot->websocket);
}

static void chttp_websocket_pool_fail(chttp_websocket_pool_impl *pool, int status) {
  size_t index;
  if (pool == NULL || pool->phase == CHTTP_WEBSOCKET_POOL_TERMINAL) return;
  pool->terminal_status = status == SALTS_OK ? SALTS_ECONNRESET : status;
  pool->phase = CHTTP_WEBSOCKET_POOL_TERMINAL;
  pool->transport_terminal = true;
  pool->receive_pending = false;
  pool->write_pending = false;
  for (index = 0u; index < pool->session_capacity; ++index) {
    chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_FREE) continue;
    chttp_websocket_pool_slot_transport_closed(slot);
    slot->stream_terminal = true;
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL;
    if (slot->terminal_status == SALTS_OK) slot->terminal_status = pool->terminal_status;
  }
}

static void chttp_websocket_pool_event_push(void *user, cnet_websocket *websocket,
                                            const cnet_websocket_event *event) {
  chttp_websocket_pool_slot *slot = (chttp_websocket_pool_slot *)user;
  chttp_websocket_pool_event_slot *event_slot;
  size_t index;
  (void)websocket;
  if (slot == NULL || event == NULL || slot->event_overflow) return;
  if (event->kind == CNET_WEBSOCKET_EVENT_CLOSE) slot->close_requested = true;
  if (slot->event_count >= slot->pool->event_capacity ||
      event->size > slot->pool->event_payload_capacity) {
    slot->event_overflow = true;
    slot->terminal_status = SALTS_ENOBUFS;
    return;
  }
  index = (slot->event_head + slot->event_count) % slot->pool->event_capacity;
  event_slot = &slot->events[index];
  if (event->size != 0u) memcpy(event_slot->payload, event->data, event->size);
  event_slot->event =
      (chttp_websocket_event){.kind = (chttp_websocket_event_kind)event->kind,
                              .message_type = (chttp_websocket_message_type)event->message_type,
                              .data = event_slot->payload,
                              .size = event->size,
                              .close_code = event->close_code};
  ++slot->event_count;
}

static int chttp_websocket_pool_write(void *user, const uint8_t *data, size_t size) {
  chttp_websocket_pool_slot *slot = (chttp_websocket_pool_slot *)user;
  if (slot == NULL || data == NULL || size == 0u || slot->pool == NULL ||
      (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPEN &&
       slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_CLOSING) ||
      slot->stream_id <= 0)
    return SALTS_EINVAL;
  if (slot->frame_size != 0u ||
      chttp_h2_proto_stream_output_pending(slot->pool->protocol, slot->stream_id))
    return SALTS_EBUSY;
  if (size > slot->pool->frame_capacity) return SALTS_EMSGSIZE;
  memcpy(slot->frame_buffer, data, size);
  slot->frame_size = size;
  if (chttp_h2_proto_submit_data(slot->pool->protocol, slot->stream_id, slot->frame_buffer, size,
                                 0) != 0) {
    slot->frame_size = 0u;
    return SALTS_ENOBUFS;
  }
  return SALTS_OK;
}

static int chttp_websocket_pool_engine_init(chttp_websocket_pool_slot *slot) {
  const cnet_websocket_config config = {.size = sizeof(config),
                                        .role = CNET_WEBSOCKET_CLIENT,
                                        .max_frame_bytes = slot->pool->max_frame_bytes,
                                        .max_message_bytes = slot->pool->max_message_bytes,
                                        .max_buffered_input_bytes =
                                            slot->pool->max_buffered_input_bytes,
                                        .write = chttp_websocket_pool_write,
                                        .on_event = chttp_websocket_pool_event_push,
                                        .user = slot};
  return cnet_websocket_init(&slot->websocket, &config);
}

static int chttp_websocket_pool_h2_begin_headers(void *user, int32_t stream_id) {
  chttp_websocket_pool_slot *slot =
      chttp_websocket_pool_stream((chttp_websocket_pool_impl *)user, stream_id);
  if (slot == NULL || slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPENING || slot->header_block_open)
    return -1;
  slot->header_block_open = true;
  slot->regular_header_seen = false;
  slot->status_seen = false;
  slot->http_status = 0u;
  return 0;
}

static int chttp_websocket_pool_h2_header(void *user, int32_t stream_id, const char *name,
                                          size_t name_size, const char *value, size_t value_size) {
  chttp_websocket_pool_slot *slot =
      chttp_websocket_pool_stream((chttp_websocket_pool_impl *)user, stream_id);
  size_t index;
  if (slot == NULL || !slot->header_block_open || name == NULL || name_size == 0u || value == NULL)
    return -1;
  for (index = 0u; index < name_size; ++index)
    if (name[index] >= 'A' && name[index] <= 'Z') return -1;
  if (name[0] == ':') {
    if (slot->regular_header_seen || slot->status_seen ||
        !chttp_websocket_pool_h2_name(name, name_size, ":status") || value_size != 3u ||
        value[0] < '1' || value[0] > '9' || value[1] < '0' || value[1] > '9' || value[2] < '0' ||
        value[2] > '9')
      return -1;
    slot->http_status = (unsigned int)(value[0] - '0') * 100u +
                        (unsigned int)(value[1] - '0') * 10u + (unsigned int)(value[2] - '0');
    slot->status_seen = true;
    return 0;
  }
  slot->regular_header_seen = true;
  if (chttp_websocket_pool_h2_name(name, name_size, "connection") ||
      chttp_websocket_pool_h2_name(name, name_size, "proxy-connection") ||
      chttp_websocket_pool_h2_name(name, name_size, "keep-alive") ||
      chttp_websocket_pool_h2_name(name, name_size, "content-length") ||
      chttp_websocket_pool_h2_name(name, name_size, "transfer-encoding") ||
      chttp_websocket_pool_h2_name(name, name_size, "te") ||
      chttp_websocket_pool_h2_name(name, name_size, "upgrade") ||
      chttp_websocket_pool_h2_name(name, name_size, "sec-websocket-accept") ||
      chttp_websocket_pool_h2_name(name, name_size, "sec-websocket-extensions") ||
      chttp_websocket_pool_h2_name(name, name_size, "sec-websocket-protocol"))
    return -1;
  return 0;
}

static int chttp_websocket_pool_h2_end_headers(void *user, int32_t stream_id, int end_stream) {
  chttp_websocket_pool_slot *slot =
      chttp_websocket_pool_stream((chttp_websocket_pool_impl *)user, stream_id);
  int status;
  if (slot == NULL || !slot->header_block_open || !slot->status_seen) return -1;
  slot->header_block_open = false;
  if (slot->http_status != 200u || end_stream) {
    slot->terminal_status = SALTS_EPROTO;
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL;
    return 0;
  }
  status = chttp_websocket_pool_engine_init(slot);
  if (status != SALTS_OK) {
    slot->terminal_status = status;
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL;
    return 0;
  }
  slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_OPEN;
  return 0;
}

static int chttp_websocket_pool_h2_data(void *user, int32_t stream_id, const uint8_t *data,
                                        size_t size) {
  chttp_websocket_pool_impl *pool = (chttp_websocket_pool_impl *)user;
  chttp_websocket_pool_slot *slot = chttp_websocket_pool_stream(pool, stream_id);
  int status = SALTS_OK;
  if (slot == NULL || (size != 0u && data == NULL)) return -1;
  if (size != 0u &&
      (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPEN ||
       slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_CLOSING) &&
      slot->websocket.impl != NULL)
    status = cnet_websocket_feed(&slot->websocket, data, size);
  if (size != 0u && (chttp_h2_proto_consume_stream(pool->protocol, stream_id, size) != 0 ||
                     chttp_h2_proto_consume_connection(pool->protocol, size) != 0))
    return -1;
  if (status != SALTS_OK || slot->event_overflow) {
    slot->terminal_status = slot->event_overflow ? SALTS_ENOBUFS : status;
    if (!slot->stream_terminal)
      (void)chttp_h2_proto_submit_rst_stream(pool->protocol, stream_id,
                                             CHTTP_H2_ERR_ENHANCE_YOUR_CALM);
    return 0;
  }
  if (chttp_h2_proto_remote_end_stream(pool->protocol, stream_id)) {
    chttp_websocket_pool_slot_transport_closed(slot);
    if (!slot->close_requested && slot->terminal_status == SALTS_OK)
      slot->terminal_status = SALTS_ECONNRESET;
  }
  return CHTTP_H2_PROTO_DATA_OK;
}

static int chttp_websocket_pool_h2_stream_close(void *user, int32_t stream_id,
                                                uint32_t error_code) {
  chttp_websocket_pool_slot *slot =
      chttp_websocket_pool_stream((chttp_websocket_pool_impl *)user, stream_id);
  if (slot == NULL) return 0;
  slot->stream_terminal = true;
  chttp_websocket_pool_slot_transport_closed(slot);
  if (error_code != CHTTP_H2_ERR_NO_ERROR && error_code != CHTTP_H2_ERR_CANCEL &&
      slot->terminal_status == SALTS_OK)
    slot->terminal_status = SALTS_ECONNRESET;
  if (error_code == CHTTP_H2_ERR_CANCEL && !slot->close_requested &&
      slot->terminal_status == SALTS_OK)
    slot->terminal_status = SALTS_ECANCELED;
  slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL;
  return 0;
}

static void chttp_websocket_pool_h2_goaway(void *user, uint32_t last_stream_id,
                                           uint32_t error_code) {
  chttp_websocket_pool_impl *pool = (chttp_websocket_pool_impl *)user;
  size_t index;
  if (pool == NULL) return;
  pool->draining = true;
  for (index = 0u; index < pool->session_capacity; ++index) {
    chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_FREE || slot->stream_id <= 0 ||
        (uint32_t)slot->stream_id <= last_stream_id)
      continue;
    slot->stream_terminal = true;
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL;
    chttp_websocket_pool_slot_transport_closed(slot);
    if (slot->terminal_status == SALTS_OK)
      slot->terminal_status = error_code == CHTTP_H2_ERR_NO_ERROR ? SALTS_ECONNRESET : SALTS_EPROTO;
  }
}

static int chttp_websocket_pool_flush(chttp_websocket_pool_impl *pool);

static int chttp_websocket_pool_receive_arm(chttp_websocket_pool_impl *pool) {
  int status;
  if (pool == NULL || pool->receive_pending || pool->transport_terminal) return SALTS_OK;
  status = cnet_receive(&pool->network, pool->connection, 1u);
  if (status == SALTS_OK) pool->receive_pending = true;
  if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) return SALTS_OK;
  return status;
}

static void chttp_websocket_pool_on_state(void *user, cnet_connection connection,
                                          cnet_connection_state state, const cnet_error *error) {
  chttp_websocket_pool_impl *pool = (chttp_websocket_pool_impl *)user;
  int status;
  if (!chttp_websocket_pool_connection_matches(pool, connection)) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    if (pool->phase != CHTTP_WEBSOCKET_POOL_CONNECTING) return;
    if (pool->secure) {
      char alpn[sizeof("h2")];
      size_t alpn_size = 0u;
      status = cnet_tls_negotiated_alpn(&pool->network, connection, alpn, sizeof(alpn), &alpn_size);
      if (status != SALTS_OK || alpn_size != sizeof("h2") - 1u ||
          memcmp(alpn, "h2", sizeof("h2") - 1u) != 0) {
        chttp_websocket_pool_fail(pool, status == SALTS_OK ? SALTS_EPROTONOSUPPORT : status);
        return;
      }
    }
    pool->phase = CHTTP_WEBSOCKET_POOL_CONNECTED;
    status = chttp_websocket_pool_receive_arm(pool);
    if (status == SALTS_OK) status = chttp_websocket_pool_flush(pool);
    if (status != SALTS_OK) chttp_websocket_pool_fail(pool, status);
    return;
  }
  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;
  chttp_websocket_pool_fail(pool, error != NULL && error->status != SALTS_OK ? error->status
                                                                             : SALTS_ECONNRESET);
}

static void chttp_websocket_pool_on_receive(void *user, cnet_connection connection,
                                            const cnet_receive_view *view) {
  chttp_websocket_pool_impl *pool = (chttp_websocket_pool_impl *)user;
  ptrdiff_t consumed;
  int status;
  if (!chttp_websocket_pool_connection_matches(pool, connection) || view == NULL ||
      pool->phase != CHTTP_WEBSOCKET_POOL_CONNECTED) {
    if (pool != NULL) chttp_websocket_pool_fail(pool, SALTS_EPROTO);
    return;
  }
  pool->receive_pending = false;
  if (view->kind != CNET_MESSAGE_BYTES) {
    chttp_websocket_pool_fail(pool, SALTS_ENOTSUP);
    return;
  }
  consumed = chttp_h2_proto_recv(pool->protocol, view->data, view->size);
  if (consumed < 0 || (size_t)consumed != view->size) {
    chttp_websocket_pool_fail(pool, SALTS_EPROTO);
    return;
  }
  status = chttp_websocket_pool_receive_arm(pool);
  if (status == SALTS_OK) status = chttp_websocket_pool_flush(pool);
  if (status != SALTS_OK) chttp_websocket_pool_fail(pool, status);
}

static void chttp_websocket_pool_on_send(void *user, cnet_connection connection, size_t size) {
  chttp_websocket_pool_impl *pool = (chttp_websocket_pool_impl *)user;
  if (!chttp_websocket_pool_connection_matches(pool, connection) || !pool->write_pending ||
      size != pool->expected_write_size) {
    if (pool != NULL) chttp_websocket_pool_fail(pool, SALTS_EPROTO);
    return;
  }
  pool->write_pending = false;
  pool->expected_write_size = 0u;
  {
    const int status = chttp_websocket_pool_flush(pool);
    if (status != SALTS_OK) chttp_websocket_pool_fail(pool, status);
  }
}

static int chttp_websocket_pool_poll(chttp_websocket_pool_impl *pool, uint64_t deadline) {
  uint32_t wait_ms = 1000u;
  size_t events = 0u;
  if (deadline != 0u) {
    const uint64_t now = salts_monotonic_ms();
    if (now >= deadline) return SALTS_ETIMEDOUT;
    wait_ms = chttp_websocket_pool_remaining(deadline);
  }
  return cnet_client_poll(&pool->network, wait_ms, &events);
}

static int chttp_websocket_pool_slot_output(chttp_websocket_pool_slot *slot) {
  cnet_websocket_state state = CNET_WEBSOCKET_OPEN;
  int status;
  if (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_FREE ||
      slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPENING || slot->stream_terminal)
    return SALTS_OK;
  if (slot->frame_size != 0u &&
      !chttp_h2_proto_stream_output_pending(slot->pool->protocol, slot->stream_id))
    slot->frame_size = 0u;
  if (slot->frame_size == 0u && slot->websocket.impl != NULL &&
      cnet_websocket_has_pending_output(&slot->websocket)) {
    status = cnet_websocket_flush(&slot->websocket);
    if (status != SALTS_OK && status != SALTS_EBUSY) return status;
  }
  if (!slot->close_requested || slot->end_submitted || slot->frame_size != 0u ||
      slot->websocket.impl == NULL || cnet_websocket_has_pending_output(&slot->websocket))
    return SALTS_OK;
  status = cnet_websocket_state_get(&slot->websocket, &state);
  if (status != SALTS_OK) return status;
  if (state != CNET_WEBSOCKET_CLOSED && state != CNET_WEBSOCKET_FAILED) return SALTS_OK;
  if (chttp_h2_proto_submit_data(slot->pool->protocol, slot->stream_id, NULL, 0u, 1) != 0)
    return SALTS_ENOBUFS;
  slot->end_submitted = true;
  return SALTS_OK;
}

static int chttp_websocket_pool_flush(chttp_websocket_pool_impl *pool) {
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  size_t index;
  int status;
  if (pool == NULL || pool->protocol == NULL || pool->transport_terminal) return SALTS_OK;
  for (index = 0u; index < pool->session_capacity; ++index) {
    status = chttp_websocket_pool_slot_output(&pool->slots[index]);
    if (status != SALTS_OK) return status;
  }
  if (pool->write_pending) return SALTS_OK;
  if (pool->wire_size == 0u) {
    wire_size = chttp_h2_proto_send(pool->protocol, &wire);
    if (wire_size < 0 || (size_t)wire_size > pool->wire_capacity) return SALTS_EPROTO;
    if (wire_size != 0) {
      memcpy(pool->wire_buffer, wire, (size_t)wire_size);
      pool->wire_size = (size_t)wire_size;
    }
  }
  if (pool->wire_size == 0u) return SALTS_OK;
  status = cnet_send(&pool->network, pool->connection, pool->wire_buffer, pool->wire_size);
  if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
  if (status != SALTS_OK) return status;
  pool->write_pending = true;
  pool->expected_write_size = pool->wire_size;
  pool->wire_size = 0u;
  return SALTS_OK;
}

static bool chttp_websocket_pool_output_pending(chttp_websocket_pool_impl *pool) {
  size_t index;
  if (pool->write_pending || pool->wire_size != 0u || chttp_h2_proto_want_write(pool->protocol))
    return true;
  for (index = 0u; index < pool->session_capacity; ++index) {
    chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_FREE || slot->stream_terminal) continue;
    if (slot->frame_size != 0u ||
        (slot->websocket.impl != NULL && cnet_websocket_has_pending_output(&slot->websocket)))
      return true;
    if (slot->close_requested && !slot->end_submitted) {
      cnet_websocket_state state = CNET_WEBSOCKET_OPEN;
      if (cnet_websocket_state_get(&slot->websocket, &state) == SALTS_OK &&
          (state == CNET_WEBSOCKET_CLOSED || state == CNET_WEBSOCKET_FAILED))
        return true;
    }
  }
  return false;
}

static int chttp_websocket_pool_drain_output(chttp_websocket_pool_impl *pool, uint64_t deadline) {
  int status;
  do {
    status = chttp_websocket_pool_flush(pool);
    if (status != SALTS_OK) return status;
    if (!chttp_websocket_pool_output_pending(pool)) return SALTS_OK;
    status = chttp_websocket_pool_receive_arm(pool);
    if (status != SALTS_OK) return status;
    status = chttp_websocket_pool_poll(pool, deadline);
    if (status != SALTS_OK) return status;
  } while (!pool->transport_terminal && pool->terminal_status == SALTS_OK);
  return pool->terminal_status == SALTS_OK ? SALTS_ECONNRESET : pool->terminal_status;
}

static int chttp_websocket_pool_h2_submit(chttp_websocket_pool_impl *pool,
                                          chttp_websocket_pool_slot *slot,
                                          const chttp_websocket_connect_options *options,
                                          const char *authority, const char *target, bool secure) {
  chttp_h2_hpack_header *headers;
  size_t header_count;
  size_t header_bytes = 0u;
  size_t name_storage_used = 0u;
  size_t index;
  int status = SALTS_OK;
  if (options->header_count > SIZE_MAX - 6u) return SALTS_ERANGE;
  header_count = options->header_count + 6u;
  if (header_count > pool->header_capacity / sizeof(*headers)) return SALTS_EMSGSIZE;
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
  for (index = 0u; index < 6u; ++index) {
    size_t field_bytes;
    if (headers[index].name_size > SIZE_MAX - headers[index].value_size - 32u) {
      status = SALTS_EMSGSIZE;
      goto done;
    }
    field_bytes = headers[index].name_size + headers[index].value_size + 32u;
    if (field_bytes > pool->header_capacity - header_bytes) {
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
    if (!chttp_websocket_pool_header_token(input->name) ||
        !chttp_websocket_pool_header_value(input->value) ||
        chttp_websocket_pool_header_owned(input->name) ||
        name_size > pool->header_capacity - name_storage_used) {
      status = SALTS_EINVAL;
      goto done;
    }
    if (name_size > SIZE_MAX - value_size - 32u ||
        name_size + value_size + 32u > pool->header_capacity - header_bytes) {
      status = SALTS_EMSGSIZE;
      goto done;
    }
    name = (char *)pool->header_name_buffer + name_storage_used;
    for (byte_index = 0u; byte_index < name_size; ++byte_index) {
      const unsigned char value = (unsigned char)input->name[byte_index];
      name[byte_index] = (char)(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
    }
    name_storage_used += name_size;
    headers[6u + index] = (chttp_h2_hpack_header){name, name_size, input->value, value_size};
    header_bytes += name_size + value_size + 32u;
  }
  if (chttp_h2_proto_submit_request_headers(pool->protocol, headers, header_count, 0,
                                            &slot->stream_id) != 0)
    status = SALTS_ENOBUFS;

done:
  free(headers);
  return status;
}

static void chttp_websocket_pool_slot_release(chttp_websocket_pool_impl *pool,
                                              chttp_websocket_pool_slot *slot) {
  chttp_websocket_pool_event_slot *events;
  unsigned char *event_payloads;
  unsigned char *frame_buffer;
  uint32_t generation;
  if (pool == NULL || slot == NULL || slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_FREE) return;
  if (slot->websocket.impl != NULL) (void)cnet_websocket_destroy(&slot->websocket);
  events = slot->events;
  event_payloads = slot->event_payloads;
  frame_buffer = slot->frame_buffer;
  generation = slot->generation;
  *slot = (chttp_websocket_pool_slot){.pool = pool,
                                      .events = events,
                                      .event_payloads = event_payloads,
                                      .frame_buffer = frame_buffer,
                                      .generation = generation,
                                      .phase = CHTTP_WEBSOCKET_POOL_SLOT_FREE};
  if (pool->active_sessions != 0u) --pool->active_sessions;
}

static chttp_websocket_pool_slot *
chttp_websocket_pool_slot_acquire(chttp_websocket_pool_impl *pool,
                                  chttp_websocket_session *out_session) {
  size_t index;
  for (index = 0u; index < pool->session_capacity; ++index) {
    chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE) continue;
    slot->generation = chttp_websocket_pool_next_generation(slot->generation);
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_OPENING;
    slot->terminal_status = SALTS_OK;
    ++pool->active_sessions;
    *out_session =
        (chttp_websocket_session){.slot = (uint32_t)(index + 1u), .generation = slot->generation};
    return slot;
  }
  return NULL;
}

int chttp_websocket_pool_init(chttp_websocket_pool *pool,
                              const chttp_websocket_pool_config *config) {
  chttp_websocket_pool_impl *impl;
  chttp_h2_proto_config protocol_config;
  chttp_h2_proto_callbacks callbacks = {0};
  size_t frame_bytes;
  size_t message_bytes;
  size_t input_bytes;
  size_t frame_capacity;
  size_t event_capacity;
  size_t event_payload_capacity;
  size_t event_slot_count;
  size_t event_payload_bytes;
  size_t frame_buffer_bytes;
  size_t index;
  int status;
  if (pool == NULL || config == NULL || pool->impl != NULL || config->size != sizeof(*config) ||
      config->client.size != sizeof(config->client) || config->session_capacity == 0u ||
      config->session_capacity > UINT32_MAX ||
      config->client.network.max_send_bytes <= CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EINVAL;
  frame_bytes = config->client.max_frame_bytes;
  if (frame_bytes == 0u) {
    frame_bytes = config->client.network.max_send_bytes - CNET_WEBSOCKET_MAX_HEADER_BYTES;
    if (frame_bytes > CHTTP_WEBSOCKET_POOL_DEFAULT_FRAME_BYTES)
      frame_bytes = CHTTP_WEBSOCKET_POOL_DEFAULT_FRAME_BYTES;
  }
  message_bytes =
      config->client.max_message_bytes == 0u ? frame_bytes : config->client.max_message_bytes;
  if (frame_bytes < CNET_WEBSOCKET_MIN_FRAME_BYTES || message_bytes < frame_bytes ||
      frame_bytes > SIZE_MAX - CNET_WEBSOCKET_MAX_HEADER_BYTES)
    return SALTS_EINVAL;
  frame_capacity = frame_bytes + CNET_WEBSOCKET_MAX_HEADER_BYTES;
  input_bytes = config->client.max_buffered_input_bytes == 0u
                    ? frame_capacity
                    : config->client.max_buffered_input_bytes;
  if (frame_capacity > config->client.network.max_send_bytes || input_bytes < frame_capacity)
    return SALTS_EMSGSIZE;
  if (input_bytes > SIZE_MAX - message_bytes ||
      input_bytes + message_bytes > SIZE_MAX - frame_capacity)
    return SALTS_ERANGE;
  event_capacity = config->client.event_capacity == 0u ? config->client.network.event_capacity
                                                       : config->client.event_capacity;
  event_payload_capacity = message_bytes > CNET_WEBSOCKET_MAX_CONTROL_BYTES
                               ? message_bytes
                               : CNET_WEBSOCKET_MAX_CONTROL_BYTES;
  if (event_capacity == 0u ||
      !chttp_websocket_pool_size_multiply(config->session_capacity, event_capacity,
                                          &event_slot_count) ||
      event_slot_count > SIZE_MAX / sizeof(chttp_websocket_pool_event_slot) ||
      !chttp_websocket_pool_size_multiply(event_slot_count, event_payload_capacity,
                                          &event_payload_bytes) ||
      !chttp_websocket_pool_size_multiply(config->session_capacity, frame_capacity,
                                          &frame_buffer_bytes) ||
      config->session_capacity > SIZE_MAX / sizeof(chttp_websocket_pool_slot))
    return SALTS_ERANGE;
  impl = (chttp_websocket_pool_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->session_capacity = config->session_capacity;
  impl->event_capacity = event_capacity;
  impl->event_payload_capacity = event_payload_capacity;
  impl->frame_capacity = frame_capacity;
  impl->wire_capacity = config->client.network.max_send_bytes;
  impl->header_capacity = config->client.max_handshake_header_bytes == 0u
                              ? CHTTP_WEBSOCKET_POOL_DEFAULT_HANDSHAKE_BYTES
                              : config->client.max_handshake_header_bytes;
  impl->max_frame_bytes = frame_bytes;
  impl->max_message_bytes = message_bytes;
  impl->max_buffered_input_bytes = input_bytes;
  protocol_config = (chttp_h2_proto_config){
      .stream_capacity = config->session_capacity,
      .output_buffer_bytes = impl->wire_capacity,
      .input_buffer_bytes = config->client.h2_input_buffer_bytes == 0u
                                ? CHTTP_WEBSOCKET_POOL_H2_INPUT_BYTES
                                : config->client.h2_input_buffer_bytes,
      .header_block_bytes = impl->header_capacity,
      .max_header_list_bytes = impl->header_capacity,
      .hpack_dynamic_table_bytes = config->client.h2_hpack_dynamic_table_bytes == 0u
                                       ? CHTTP_WEBSOCKET_POOL_H2_HPACK_BYTES
                                       : config->client.h2_hpack_dynamic_table_bytes,
      .max_hpack_string_bytes = impl->header_capacity,
      .max_settings_count = config->client.h2_max_settings_count == 0u
                                ? CHTTP_WEBSOCKET_POOL_H2_SETTINGS_COUNT
                                : config->client.h2_max_settings_count};
  if (protocol_config.input_buffer_bytes <
          CHTTP_WEBSOCKET_POOL_H2_FRAME_HEADER_BYTES + CHTTP_WEBSOCKET_POOL_H2_MIN_FRAME_BYTES ||
      protocol_config.input_buffer_bytes > PTRDIFF_MAX ||
      protocol_config.hpack_dynamic_table_bytes > UINT32_MAX ||
      protocol_config.max_settings_count > SIZE_MAX / sizeof(uint32_t) ||
      !chttp_h2_proto_config_valid(&protocol_config)) {
    free(impl);
    return SALTS_EMSGSIZE;
  }
  impl->slots = (chttp_websocket_pool_slot *)calloc(config->session_capacity, sizeof(*impl->slots));
  impl->event_slots =
      (chttp_websocket_pool_event_slot *)calloc(event_slot_count, sizeof(*impl->event_slots));
  impl->event_payloads = (unsigned char *)malloc(event_payload_bytes);
  impl->frame_buffers = (unsigned char *)malloc(frame_buffer_bytes);
  impl->wire_buffer = (unsigned char *)malloc(impl->wire_capacity);
  impl->header_name_buffer = (unsigned char *)malloc(impl->header_capacity);
  if (impl->slots == NULL || impl->event_slots == NULL || impl->event_payloads == NULL ||
      impl->frame_buffers == NULL || impl->wire_buffer == NULL ||
      impl->header_name_buffer == NULL) {
    status = SALTS_ENOMEM;
    goto fail;
  }
  for (index = 0u; index < config->session_capacity; ++index) {
    size_t event_index;
    chttp_websocket_pool_slot *slot = &impl->slots[index];
    slot->pool = impl;
    slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_FREE;
    slot->events = impl->event_slots + index * event_capacity;
    slot->event_payloads = impl->event_payloads + index * event_capacity * event_payload_capacity;
    slot->frame_buffer = impl->frame_buffers + index * frame_capacity;
    for (event_index = 0u; event_index < event_capacity; ++event_index)
      slot->events[event_index].payload =
          slot->event_payloads + event_index * event_payload_capacity;
  }
  callbacks = (chttp_h2_proto_callbacks){.user_data = impl,
                                         .on_begin_headers = chttp_websocket_pool_h2_begin_headers,
                                         .on_header = chttp_websocket_pool_h2_header,
                                         .on_end_headers = chttp_websocket_pool_h2_end_headers,
                                         .on_data = chttp_websocket_pool_h2_data,
                                         .on_stream_close = chttp_websocket_pool_h2_stream_close,
                                         .on_goaway = chttp_websocket_pool_h2_goaway};
  impl->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_CLIENT, &protocol_config, &callbacks);
  if (impl->protocol == NULL) {
    status = SALTS_ENOMEM;
    goto fail;
  }
  status = cnet_client_init(&impl->network, &config->client.network);
  if (status != SALTS_OK) goto fail;
  pool->impl = impl;
  return SALTS_OK;

fail:
  chttp_h2_proto_destroy(impl->protocol);
  free(impl->header_name_buffer);
  free(impl->wire_buffer);
  free(impl->frame_buffers);
  free(impl->event_payloads);
  free(impl->event_slots);
  free(impl->slots);
  free(impl);
  return status;
}

static int chttp_websocket_pool_connect(chttp_websocket_pool_impl *pool, const char *transport,
                                        const char *authority, bool secure,
                                        chttp_tls_profile_impl *tls_profile, uint64_t deadline) {
  const cnet_observer observer = {.on_state = chttp_websocket_pool_on_state,
                                  .on_receive = chttp_websocket_pool_on_receive,
                                  .on_send = chttp_websocket_pool_on_send,
                                  .user = pool};
  const cnet_connect_options options = {
      .uri = transport, .observer = observer, .tls_client = chttp_tls_profile_client(tls_profile)};
  int status;
  pool->secure = secure;
  pool->tls_profile = tls_profile;
  memcpy(pool->transport, transport, strlen(transport) + 1u);
  memcpy(pool->authority, authority, strlen(authority) + 1u);
  pool->phase = CHTTP_WEBSOCKET_POOL_CONNECTING;
  status = cnet_connect(&pool->network, &options, &pool->connection);
  if (status != SALTS_OK) {
    pool->phase = CHTTP_WEBSOCKET_POOL_DISCONNECTED;
    pool->tls_profile = NULL;
    return status;
  }
  while (pool->phase == CHTTP_WEBSOCKET_POOL_CONNECTING && !pool->transport_terminal) {
    status = chttp_websocket_pool_poll(pool, deadline);
    if (status != SALTS_OK) return status;
  }
  if (pool->phase != CHTTP_WEBSOCKET_POOL_CONNECTED)
    return pool->terminal_status == SALTS_OK ? SALTS_ECONNRESET : pool->terminal_status;
  while (!chttp_h2_proto_peer_settings_received(pool->protocol) && !pool->transport_terminal) {
    status = chttp_websocket_pool_receive_arm(pool);
    if (status == SALTS_OK) status = chttp_websocket_pool_flush(pool);
    if (status != SALTS_OK) return status;
    status = chttp_websocket_pool_poll(pool, deadline);
    if (status != SALTS_OK) return status;
  }
  if (pool->transport_terminal)
    return pool->terminal_status == SALTS_OK ? SALTS_ECONNRESET : pool->terminal_status;
  if (chttp_h2_proto_peer_enable_connect_protocol(pool->protocol) != 1u) {
    pool->draining = true;
    return SALTS_EPROTONOSUPPORT;
  }
  return SALTS_OK;
}

int chttp_websocket_pool_open(chttp_websocket_pool *pool,
                              const chttp_websocket_connect_options *options,
                              chttp_websocket_session *out_session, unsigned int *out_http_status) {
  chttp_websocket_pool_impl *impl;
  chttp_websocket_pool_slot *slot = NULL;
  chttp_websocket_session session = {0};
  chttp_tls_profile_impl *tls_profile = NULL;
  char transport[CHTTP_WEBSOCKET_POOL_TRANSPORT_BYTES];
  char authority[CHTTP_WEBSOCKET_POOL_AUTHORITY_BYTES];
  char target[CHTTP_WEBSOCKET_POOL_TARGET_BYTES];
  bool secure = false;
  bool tls_transferred = false;
  uint64_t deadline;
  int status;
  if (out_session != NULL) *out_session = (chttp_websocket_session){0};
  if (out_http_status != NULL) *out_http_status = 0u;
  if (pool == NULL || pool->impl == NULL || options == NULL || out_session == NULL ||
      out_http_status == NULL || options->size != sizeof(*options) || options->uri == NULL ||
      (options->header_count != 0u && options->headers == NULL))
    return SALTS_EINVAL;
  if (options->protocol != CHTTP_HTTP_2) return SALTS_EPROTONOSUPPORT;
  impl = (chttp_websocket_pool_impl *)pool->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  if (impl->phase == CHTTP_WEBSOCKET_POOL_TERMINAL || impl->draining) return SALTS_ESHUTDOWN;
  impl->operation_active = true;
  deadline = chttp_websocket_pool_deadline(options->timeout_ms);
  status = chttp_websocket_pool_uri(options->uri, &secure, transport, sizeof(transport), authority,
                                    sizeof(authority), target, sizeof(target));
  if (status != SALTS_OK) goto done;
  if ((!secure && options->tls != NULL) ||
      (secure && options->tls != NULL && options->tls->impl == NULL)) {
    status = SALTS_EINVAL;
    goto done;
  }
  status = chttp_tls_profile_acquire(options->tls, &tls_profile);
  if (status != SALTS_OK) goto done;
  if (secure && (tls_profile == NULL || chttp_tls_profile_protocol(tls_profile) != CHTTP_HTTP_2)) {
    status = SALTS_EPROTONOSUPPORT;
    goto done;
  }
  if (impl->phase == CHTTP_WEBSOCKET_POOL_DISCONNECTED) {
    status =
        chttp_websocket_pool_connect(impl, transport, authority, secure, tls_profile, deadline);
    tls_transferred = impl->tls_profile == tls_profile;
    if (status != SALTS_OK) {
      if (impl->phase != CHTTP_WEBSOCKET_POOL_DISCONNECTED) {
        impl->draining = true;
        if (!impl->transport_terminal && impl->connection.generation != 0u)
          (void)cnet_close(&impl->network, impl->connection);
      }
      goto done;
    }
  } else if (impl->secure != secure || strcmp(impl->transport, transport) != 0 ||
             strcmp(impl->authority, authority) != 0 || impl->tls_profile != tls_profile) {
    status = SALTS_EINVAL;
    goto done;
  }
  if (impl->active_sessions >= impl->session_capacity ||
      impl->active_sessions >= chttp_h2_proto_peer_max_concurrent_streams(impl->protocol)) {
    status = SALTS_ENOBUFS;
    goto done;
  }
  slot = chttp_websocket_pool_slot_acquire(impl, &session);
  if (slot == NULL) {
    status = SALTS_ENOBUFS;
    goto done;
  }
  status = chttp_websocket_pool_h2_submit(impl, slot, options, authority, target, secure);
  if (status != SALTS_OK) goto release_slot;
  status = chttp_websocket_pool_drain_output(impl, deadline);
  if (status != SALTS_OK) goto release_slot;
  while (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPENING && !impl->transport_terminal) {
    status = chttp_websocket_pool_receive_arm(impl);
    if (status != SALTS_OK) goto release_slot;
    status = chttp_websocket_pool_poll(impl, deadline);
    if (status != SALTS_OK) goto release_slot;
    status = chttp_websocket_pool_flush(impl);
    if (status != SALTS_OK) goto release_slot;
  }
  *out_http_status = slot->http_status;
  if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPEN) {
    status = slot->terminal_status == SALTS_OK ? SALTS_EPROTO : slot->terminal_status;
    goto release_slot;
  }
  status = chttp_websocket_pool_drain_output(impl, deadline);
  if (status != SALTS_OK) goto release_slot;
  *out_session = session;
  status = SALTS_OK;
  goto done;

release_slot:
  *out_http_status = slot->http_status;
  if (slot->stream_id > 0 && !slot->stream_terminal) {
    slot->close_requested = true;
    (void)chttp_h2_proto_submit_rst_stream(impl->protocol, slot->stream_id, CHTTP_H2_ERR_CANCEL);
    (void)chttp_websocket_pool_flush(impl);
  }
  chttp_websocket_pool_slot_release(impl, slot);

done:
  if (tls_profile != NULL && !tls_transferred) chttp_tls_profile_release(tls_profile);
  impl->operation_active = false;
  return status;
}

typedef int (*chttp_websocket_pool_send_fn)(cnet_websocket *websocket, const void *data,
                                            size_t size);

static int chttp_websocket_pool_send(chttp_websocket_pool *pool, chttp_websocket_session session,
                                     chttp_websocket_pool_send_fn send, const void *data,
                                     size_t size, uint32_t timeout_ms) {
  chttp_websocket_pool_impl *impl;
  chttp_websocket_pool_slot *slot;
  uint64_t deadline;
  int status;
  if (pool == NULL || pool->impl == NULL || send == NULL || (data == NULL && size != 0u))
    return SALTS_EINVAL;
  impl = (chttp_websocket_pool_impl *)pool->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  slot = chttp_websocket_pool_session(impl, session);
  if (slot == NULL) return SALTS_ENOENT;
  if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPEN || slot->terminal_status != SALTS_OK)
    return SALTS_ESHUTDOWN;
  impl->operation_active = true;
  deadline = chttp_websocket_pool_deadline(timeout_ms);
  status = chttp_websocket_pool_drain_output(impl, deadline);
  if (status == SALTS_OK) status = send(&slot->websocket, data, size);
  if (status == SALTS_OK) status = chttp_websocket_pool_drain_output(impl, deadline);
  if (status == SALTS_OK && slot->terminal_status != SALTS_OK) status = slot->terminal_status;
  impl->operation_active = false;
  return status;
}

int chttp_websocket_pool_send_text(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms) {
  return chttp_websocket_pool_send(pool, session, cnet_websocket_send_text, data, size, timeout_ms);
}

int chttp_websocket_pool_send_binary(chttp_websocket_pool *pool, chttp_websocket_session session,
                                     const void *data, size_t size, uint32_t timeout_ms) {
  return chttp_websocket_pool_send(pool, session, cnet_websocket_send_binary, data, size,
                                   timeout_ms);
}

int chttp_websocket_pool_send_ping(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms) {
  return chttp_websocket_pool_send(pool, session, cnet_websocket_send_ping, data, size, timeout_ms);
}

int chttp_websocket_pool_send_pong(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms) {
  return chttp_websocket_pool_send(pool, session, cnet_websocket_send_pong, data, size, timeout_ms);
}

int chttp_websocket_pool_receive(chttp_websocket_pool *pool, chttp_websocket_session session,
                                 uint32_t timeout_ms, chttp_websocket_event *out_event) {
  chttp_websocket_pool_impl *impl;
  chttp_websocket_pool_slot *slot;
  uint64_t deadline;
  int status = SALTS_OK;
  if (pool == NULL || pool->impl == NULL || out_event == NULL) return SALTS_EINVAL;
  *out_event = (chttp_websocket_event){0};
  impl = (chttp_websocket_pool_impl *)pool->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  slot = chttp_websocket_pool_session(impl, session);
  if (slot == NULL) return SALTS_ENOENT;
  if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPEN && slot->event_count == 0u)
    return slot->terminal_status == SALTS_OK ? SALTS_ESHUTDOWN : slot->terminal_status;
  impl->operation_active = true;
  deadline = chttp_websocket_pool_deadline(timeout_ms);
  while (slot->event_count == 0u && slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPEN &&
         !impl->transport_terminal && slot->terminal_status == SALTS_OK) {
    status = chttp_websocket_pool_receive_arm(impl);
    if (status != SALTS_OK) break;
    status = chttp_websocket_pool_poll(impl, deadline);
    if (status != SALTS_OK) break;
  }
  if (status == SALTS_OK && slot->event_count != 0u) {
    *out_event = slot->events[slot->event_head].event;
    slot->event_head = (slot->event_head + 1u) % impl->event_capacity;
    --slot->event_count;
    status = chttp_websocket_pool_flush(impl);
  } else if (status == SALTS_OK) {
    status = slot->terminal_status == SALTS_OK ? SALTS_ECONNRESET : slot->terminal_status;
  }
  impl->operation_active = false;
  return status;
}

int chttp_websocket_pool_close(chttp_websocket_pool *pool, chttp_websocket_session session,
                               uint16_t code, const void *reason, size_t reason_size,
                               uint32_t timeout_ms) {
  chttp_websocket_pool_impl *impl;
  chttp_websocket_pool_slot *slot;
  uint64_t deadline;
  int result;
  int status = SALTS_OK;
  if (pool == NULL || pool->impl == NULL || (reason == NULL && reason_size != 0u))
    return SALTS_EINVAL;
  impl = (chttp_websocket_pool_impl *)pool->impl;
  if (impl->operation_active) return SALTS_EBUSY;
  slot = chttp_websocket_pool_session(impl, session);
  if (slot == NULL) return SALTS_ENOENT;
  impl->operation_active = true;
  deadline = chttp_websocket_pool_deadline(timeout_ms);
  if (slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPEN) {
    status = chttp_websocket_pool_drain_output(impl, deadline);
    if (status == SALTS_OK && !slot->stream_terminal &&
        slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPEN && !slot->close_requested)
      status = cnet_websocket_close(&slot->websocket, code, reason, reason_size);
    if (status == SALTS_OK && !slot->stream_terminal &&
        slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_OPEN) {
      slot->close_requested = true;
      slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_CLOSING;
    }
  } else if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_CLOSING &&
             slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL) {
    status = SALTS_EALREADY;
  }
  while (status == SALTS_OK && !slot->stream_terminal && !impl->transport_terminal) {
    status = chttp_websocket_pool_drain_output(impl, deadline);
    if (status != SALTS_OK || slot->stream_terminal) break;
    status = chttp_websocket_pool_receive_arm(impl);
    if (status != SALTS_OK) break;
    status = chttp_websocket_pool_poll(impl, deadline);
  }
  if (status == SALTS_ETIMEDOUT) {
    impl->operation_active = false;
    return status;
  }
  result = status;
  if (result == SALTS_OK && slot->terminal_status != SALTS_OK) result = slot->terminal_status;
  if (slot->stream_terminal || slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL ||
      impl->transport_terminal)
    chttp_websocket_pool_slot_release(impl, slot);
  impl->operation_active = false;
  return result;
}

static bool chttp_websocket_pool_all_terminal(const chttp_websocket_pool_impl *pool) {
  size_t index;
  for (index = 0u; index < pool->session_capacity; ++index) {
    const chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE && !slot->stream_terminal &&
        slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL)
      return false;
  }
  return true;
}

static void chttp_websocket_pool_release_terminal(chttp_websocket_pool_impl *pool) {
  size_t index;
  for (index = 0u; index < pool->session_capacity; ++index) {
    chttp_websocket_pool_slot *slot = &pool->slots[index];
    if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE &&
        (slot->stream_terminal || slot->phase == CHTTP_WEBSOCKET_POOL_SLOT_TERMINAL ||
         pool->transport_terminal))
      chttp_websocket_pool_slot_release(pool, slot);
  }
}

int chttp_websocket_pool_destroy(chttp_websocket_pool *pool, uint32_t timeout_ms) {
  chttp_websocket_pool_impl *impl;
  uint64_t deadline;
  size_t index;
  int first_status = SALTS_OK;
  int status;
  if (pool == NULL) return SALTS_EINVAL;
  impl = (chttp_websocket_pool_impl *)pool->impl;
  if (impl == NULL) return SALTS_OK;
  if (impl->operation_active) return SALTS_EBUSY;
  impl->operation_active = true;
  impl->draining = true;
  deadline = chttp_websocket_pool_deadline(timeout_ms);
  if (impl->phase == CHTTP_WEBSOCKET_POOL_CONNECTED && impl->active_sessions != 0u) {
    status = chttp_websocket_pool_drain_output(impl, deadline);
    if (status != SALTS_OK && status != SALTS_ETIMEDOUT) first_status = status;
    if (status == SALTS_ETIMEDOUT) {
      impl->operation_active = false;
      return status;
    }
    for (index = 0u; index < impl->session_capacity; ++index) {
      chttp_websocket_pool_slot *slot = &impl->slots[index];
      if (slot->phase != CHTTP_WEBSOCKET_POOL_SLOT_OPEN) continue;
      status = cnet_websocket_close(&slot->websocket, 1001u, NULL, 0u);
      if (status == SALTS_OK) {
        slot->close_requested = true;
        slot->phase = CHTTP_WEBSOCKET_POOL_SLOT_CLOSING;
      } else if (first_status == SALTS_OK) {
        first_status = status;
      }
    }
    while (!impl->transport_terminal && !chttp_websocket_pool_all_terminal(impl)) {
      status = chttp_websocket_pool_drain_output(impl, deadline);
      if (status != SALTS_OK) break;
      if (chttp_websocket_pool_all_terminal(impl)) break;
      status = chttp_websocket_pool_receive_arm(impl);
      if (status != SALTS_OK) break;
      status = chttp_websocket_pool_poll(impl, deadline);
      if (status != SALTS_OK) break;
    }
    if (status == SALTS_ETIMEDOUT) {
      impl->operation_active = false;
      return status;
    }
    if (first_status == SALTS_OK && status != SALTS_OK) first_status = status;
  }
  chttp_websocket_pool_release_terminal(impl);
  if (!impl->transport_terminal && impl->connection.generation != 0u) {
    status = cnet_close(&impl->network, impl->connection);
    if (first_status == SALTS_OK && status != SALTS_OK && status != SALTS_EALREADY &&
        status != SALTS_ENOENT && status != SALTS_ESHUTDOWN)
      first_status = status;
  }
  impl->operation_active = false;
  status = cnet_client_stop(&impl->network,
                            deadline == 0u ? 0u : chttp_websocket_pool_remaining(deadline));
  if (status == SALTS_ETIMEDOUT) return status;
  if (first_status == SALTS_OK && status != SALTS_OK && status != SALTS_EALREADY)
    first_status = status;
  status = cnet_client_destroy(&impl->network);
  if (status != SALTS_OK) return first_status == SALTS_OK ? status : first_status;
  for (index = 0u; index < impl->session_capacity; ++index)
    if (impl->slots[index].phase != CHTTP_WEBSOCKET_POOL_SLOT_FREE)
      chttp_websocket_pool_slot_release(impl, &impl->slots[index]);
  chttp_h2_proto_destroy(impl->protocol);
  chttp_tls_profile_release(impl->tls_profile);
  free(impl->header_name_buffer);
  free(impl->wire_buffer);
  free(impl->frame_buffers);
  free(impl->event_payloads);
  free(impl->event_slots);
  free(impl->slots);
  free(impl);
  pool->impl = NULL;
  return first_status;
}
