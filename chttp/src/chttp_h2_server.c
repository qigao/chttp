#include "chttp_h2_server.h"

#include "chttp_jwt_internal.h"
#include "chttp_server_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_H2_SERVER_FRAME_HEADER_BYTES = 9, CHTTP_H2_SERVER_MIN_FRAME_BYTES = 16 * 1024 };

static const uint8_t CHTTP_H2_SERVER_DRAIN_PING[8] = {'c', 'h', 't', 't', 'p', 'h', '2', '!'};

typedef struct chttp_h2_server_stream {
  struct chttp_h2_server_connection *owner;
  chttp_server_request_state request_state;
  chttp_server_response_builder deferred_builder;
  chttp_server_response deferred_response;
  chttp_server_deferred_target deferred_target;
  atomic_uint_fast64_t deferred_token;
  chttp_server_websocket_peer websocket_peer;
  chttp_header *headers;
  chttp_h2_hpack_header *response_headers;
  char *header_storage;
  char *target_storage;
  char *authority;
  unsigned char *body;
  unsigned char *websocket_output;
  size_t header_storage_capacity;
  size_t header_storage_used;
  size_t header_count;
  size_t body_size;
  size_t body_capacity;
  size_t response_header_capacity;
  size_t websocket_output_capacity;
  size_t websocket_output_size;
  int32_t stream_id;
  chttp_method method;
  bool active;
  bool header_block_open;
  bool trailers;
  bool regular_header_seen;
  bool method_seen;
  bool extended_connect;
  bool protocol_seen;
  bool websocket_version_seen;
  bool websocket_end_submitted;
  bool scheme_seen;
  bool path_seen;
  bool authority_seen;
  bool content_length_seen;
  bool response_submitted;
  bool protocol_closed;
  size_t content_length;
} chttp_h2_server_stream;

struct chttp_h2_server_connection {
  chttp_server_connection *connection;
  chttp_h2_proto *protocol;
  chttp_h2_proto_config protocol_config;
  chttp_h2_server_stream *streams;
  size_t stream_capacity;
  size_t active_streams;
  bool draining;
  bool drain_ping_sent;
  bool drain_ping_acked;
};

static bool chttp_h2_server_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || left > SIZE_MAX - right) return false;
  *out = left + right;
  return true;
}

static bool chttp_h2_server_multiply(size_t left, size_t right, size_t *out) {
  if (out == NULL || (right != 0u && left > SIZE_MAX / right)) return false;
  *out = left * right;
  return true;
}

static unsigned char chttp_h2_server_ascii_lower(unsigned char value) {
  return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
             ? (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'))
             : value;
}

static bool chttp_h2_server_ascii_equal_n(const char *left, size_t left_size, const char *right) {
  size_t index;
  const size_t right_size = strlen(right);
  if (left == NULL || left_size != right_size) return false;
  for (index = 0u; index < left_size; ++index)
    if (chttp_h2_server_ascii_lower((unsigned char)left[index]) !=
        chttp_h2_server_ascii_lower((unsigned char)right[index]))
      return false;
  return true;
}

static bool chttp_h2_server_header_name_byte(unsigned char value) {
  if ((value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
      (value >= (unsigned char)'0' && value <= (unsigned char)'9'))
    return true;
  return value == (unsigned char)'!' || value == (unsigned char)'#' ||
         value == (unsigned char)'$' || value == (unsigned char)'%' ||
         value == (unsigned char)'&' || value == (unsigned char)'\'' ||
         value == (unsigned char)'*' || value == (unsigned char)'+' ||
         value == (unsigned char)'-' || value == (unsigned char)'.' ||
         value == (unsigned char)'^' || value == (unsigned char)'_' ||
         value == (unsigned char)'`' || value == (unsigned char)'|' || value == (unsigned char)'~';
}

static bool chttp_h2_server_header_value(const char *value, size_t size) {
  size_t index;
  if (value == NULL) return false;
  if (size != 0u &&
      (value[0] == ' ' || value[0] == '\t' || value[size - 1u] == ' ' || value[size - 1u] == '\t'))
    return false;
  for (index = 0u; index < size; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if ((byte < 0x20u && byte != (unsigned char)'\t') || byte == 0x7fu) return false;
  }
  return true;
}

static bool chttp_h2_server_forbidden_header(const char *name, size_t size) {
  return chttp_h2_server_ascii_equal_n(name, size, "connection") ||
         chttp_h2_server_ascii_equal_n(name, size, "proxy-connection") ||
         chttp_h2_server_ascii_equal_n(name, size, "keep-alive") ||
         chttp_h2_server_ascii_equal_n(name, size, "transfer-encoding") ||
         chttp_h2_server_ascii_equal_n(name, size, "upgrade");
}

static int chttp_h2_server_decimal(const char *value, size_t size, size_t *out_value) {
  size_t result = 0u;
  size_t index;
  if (value == NULL || size == 0u || out_value == NULL) return SALTS_EPROTO;
  for (index = 0u; index < size; ++index) {
    const unsigned char digit = (unsigned char)value[index];
    if (digit < (unsigned char)'0' || digit > (unsigned char)'9' ||
        result > (SIZE_MAX - (size_t)(digit - (unsigned char)'0')) / 10u)
      return SALTS_EPROTO;
    result = result * 10u + (size_t)(digit - (unsigned char)'0');
  }
  *out_value = result;
  return SALTS_OK;
}

static chttp_method chttp_h2_server_method(const char *value, size_t size,
                                           bool *out_extended_connect) {
  static const struct {
    const char *name;
    chttp_method method;
  } methods[] = {{"GET", CHTTP_METHOD_GET},        {"HEAD", CHTTP_METHOD_HEAD},
                 {"POST", CHTTP_METHOD_POST},      {"PUT", CHTTP_METHOD_PUT},
                 {"DELETE", CHTTP_METHOD_DELETE},  {"PATCH", CHTTP_METHOD_PATCH},
                 {"OPTIONS", CHTTP_METHOD_OPTIONS}};
  size_t index;
  if (out_extended_connect == NULL) return (chttp_method)0;
  *out_extended_connect = false;
  if (size == sizeof("CONNECT") - 1u && memcmp(value, "CONNECT", size) == 0) {
    *out_extended_connect = true;
    return CHTTP_METHOD_CONNECT;
  }
  for (index = 0u; index < sizeof(methods) / sizeof(methods[0]); ++index)
    if (strlen(methods[index].name) == size && memcmp(value, methods[index].name, size) == 0)
      return methods[index].method;
  return (chttp_method)0;
}

static int chttp_h2_server_config_disabled(const chttp_server_config *config) {
  return config->h2_stream_capacity == 0u && config->h2_input_buffer_bytes == 0u &&
                 config->h2_output_buffer_bytes == 0u &&
                 config->h2_hpack_dynamic_table_bytes == 0u && config->h2_max_settings_count == 0u
             ? SALTS_OK
             : SALTS_EINVAL;
}

int chttp_h2_server_config_validate(const chttp_server_config *config,
                                    chttp_h2_proto_config *out_protocol_config) {
  chttp_h2_proto_config protocol_config;
  size_t header_storage_bytes;
  size_t target_storage_bytes;
  size_t request_header_block_bytes;
  size_t response_header_block_bytes;
  size_t response_header_count;
  size_t response_header_array_bytes;
  size_t generated_response_field_bytes;
  size_t per_stream_bytes;
  size_t all_stream_bytes;
  if (config == NULL || out_protocol_config == NULL ||
      (config->enable_http2 != 0 && config->enable_http2 != 1))
    return SALTS_EINVAL;
  *out_protocol_config = (chttp_h2_proto_config){0};
  if (!config->enable_http2) return chttp_h2_server_config_disabled(config);
  if (config->h2_stream_capacity == 0u || config->h2_stream_capacity > UINT32_MAX ||
      config->h2_input_buffer_bytes <
          CHTTP_H2_SERVER_FRAME_HEADER_BYTES + CHTTP_H2_SERVER_MIN_FRAME_BYTES ||
      config->h2_input_buffer_bytes > PTRDIFF_MAX || config->h2_output_buffer_bytes == 0u ||
      config->h2_output_buffer_bytes > config->network.max_send_bytes ||
      config->h2_output_buffer_bytes < CHTTP_H2_SERVER_FRAME_HEADER_BYTES ||
      config->max_header_bytes >
          config->h2_output_buffer_bytes - CHTTP_H2_SERVER_FRAME_HEADER_BYTES ||
      config->h2_hpack_dynamic_table_bytes == 0u ||
      config->h2_hpack_dynamic_table_bytes > UINT32_MAX || config->h2_max_settings_count == 0u ||
      config->h2_max_settings_count > SIZE_MAX / sizeof(uint32_t))
    return SALTS_EMSGSIZE;
  if (!chttp_h2_server_multiply(config->max_header_count, 2u, &header_storage_bytes) ||
      !chttp_h2_server_add(header_storage_bytes, config->max_header_bytes, &header_storage_bytes) ||
      !chttp_h2_server_add(config->max_target_bytes, 1u, &target_storage_bytes) ||
      !chttp_h2_server_multiply(target_storage_bytes, 2u, &target_storage_bytes) ||
      !chttp_h2_server_add(config->max_response_header_count, 2u, &response_header_count) ||
      !chttp_h2_server_multiply(response_header_count, sizeof(chttp_h2_hpack_header),
                                &response_header_array_bytes) ||
      !chttp_h2_server_multiply(config->max_header_count, CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES,
                                &request_header_block_bytes) ||
      !chttp_h2_server_add(request_header_block_bytes, config->max_header_bytes,
                           &request_header_block_bytes) ||
      !chttp_h2_server_add(request_header_block_bytes, CHTTP_H2_PROTO_HPACK_PENDING_UPDATE_BYTES,
                           &request_header_block_bytes) ||
      !chttp_h2_server_multiply(response_header_count, CHTTP_H2_PROTO_HPACK_FIELD_OVERHEAD_BYTES,
                                &response_header_block_bytes) ||
      !chttp_h2_server_add(response_header_block_bytes, config->max_response_header_bytes,
                           &response_header_block_bytes) ||
      !chttp_h2_server_add(
          sizeof(":status") - 1u + 3u + sizeof("content-length") - 1u + 3u * sizeof(size_t),
          CHTTP_H2_PROTO_HPACK_PENDING_UPDATE_BYTES, &generated_response_field_bytes) ||
      !chttp_h2_server_add(response_header_block_bytes, generated_response_field_bytes,
                           &response_header_block_bytes) ||
      !chttp_h2_server_add(sizeof(chttp_h2_server_stream), header_storage_bytes,
                           &per_stream_bytes) ||
      !chttp_h2_server_add(per_stream_bytes, target_storage_bytes, &per_stream_bytes) ||
      !chttp_h2_server_add(per_stream_bytes, config->max_header_bytes + 1u, &per_stream_bytes) ||
      !chttp_h2_server_add(per_stream_bytes, config->max_request_body_bytes, &per_stream_bytes) ||
      !chttp_h2_server_add(per_stream_bytes, config->network.max_send_bytes, &per_stream_bytes) ||
      !chttp_h2_server_add(per_stream_bytes, response_header_array_bytes, &per_stream_bytes) ||
      !chttp_h2_server_multiply(per_stream_bytes, config->h2_stream_capacity, &all_stream_bytes) ||
      !chttp_h2_server_multiply(all_stream_bytes, config->network.connection_capacity,
                                &all_stream_bytes))
    return SALTS_ERANGE;
  protocol_config = (chttp_h2_proto_config){
      .stream_capacity = config->h2_stream_capacity,
      .output_buffer_bytes = config->h2_output_buffer_bytes,
      .input_buffer_bytes = config->h2_input_buffer_bytes,
      .header_block_bytes = request_header_block_bytes > response_header_block_bytes
                                ? request_header_block_bytes
                                : response_header_block_bytes,
      .max_header_list_bytes = config->max_header_bytes,
      .hpack_dynamic_table_bytes = config->h2_hpack_dynamic_table_bytes,
      .max_hpack_string_bytes = config->max_header_bytes > config->max_response_header_bytes
                                    ? config->max_header_bytes
                                    : config->max_response_header_bytes,
      .max_settings_count = config->h2_max_settings_count};
  if (!chttp_h2_proto_config_valid(&protocol_config)) return SALTS_EMSGSIZE;
  *out_protocol_config = protocol_config;
  return SALTS_OK;
}

static void chttp_h2_server_stream_reset(chttp_h2_server_stream *stream) {
  if (stream == NULL) return;
  if (stream->websocket_peer.engine.impl != NULL) {
    chttp_server_websocket_peer_transport_closed(&stream->websocket_peer);
    chttp_server_websocket_peer_reset(&stream->websocket_peer);
  }
  chttp_server_request_state_reset(&stream->request_state);
  chttp_server_response_builder_reset(&stream->deferred_builder);
  chttp_server_buffer_release(stream->owner->connection->server, stream->body,
                              stream->body_capacity);
  stream->body = NULL;
  stream->body_capacity = 0u;
  chttp_server_buffer_release(stream->owner->connection->server, stream->websocket_output,
                              stream->websocket_output_capacity);
  stream->websocket_output = NULL;
  stream->websocket_output_capacity = 0u;
  stream->header_storage_used = 0u;
  stream->header_count = 0u;
  stream->body_size = 0u;
  stream->websocket_output_size = 0u;
  stream->stream_id = 0;
  stream->method = (chttp_method)0;
  stream->active = false;
  stream->header_block_open = false;
  stream->trailers = false;
  stream->regular_header_seen = false;
  stream->method_seen = false;
  stream->extended_connect = false;
  stream->protocol_seen = false;
  stream->websocket_version_seen = false;
  stream->websocket_end_submitted = false;
  stream->scheme_seen = false;
  stream->path_seen = false;
  stream->authority_seen = false;
  stream->content_length_seen = false;
  stream->response_submitted = false;
  stream->protocol_closed = false;
  stream->content_length = 0u;
  if (stream->target_storage != NULL) stream->target_storage[0] = '\0';
  if (stream->authority != NULL) stream->authority[0] = '\0';
}

static void chttp_h2_server_stream_destroy(chttp_h2_server_stream *stream) {
  if (stream == NULL) return;
  chttp_server_request_state_destroy(&stream->request_state);
  chttp_server_response_builder_destroy(&stream->deferred_builder);
  chttp_server_websocket_peer_reset(&stream->websocket_peer);
  if (stream->owner != NULL) {
    chttp_server_buffer_release(stream->owner->connection->server, stream->websocket_output,
                                stream->websocket_output_capacity);
    chttp_server_buffer_release(stream->owner->connection->server, stream->body,
                                stream->body_capacity);
  } else {
    free(stream->websocket_output);
    free(stream->body);
  }
  free(stream->authority);
  free(stream->target_storage);
  free(stream->header_storage);
  free(stream->response_headers);
  *stream = (chttp_h2_server_stream){0};
}

static int chttp_h2_server_stream_init(chttp_h2_server_stream *stream,
                                       chttp_h2_server_connection *owner) {
  const chttp_server_config *config = &owner->connection->server->config;
  size_t header_terminators;
  size_t target_stride;
  int status;
  if (!chttp_h2_server_multiply(config->max_header_count, 2u, &header_terminators) ||
      !chttp_h2_server_add(config->max_header_bytes, header_terminators,
                           &stream->header_storage_capacity) ||
      !chttp_h2_server_add(config->max_target_bytes, 1u, &target_stride) ||
      config->max_response_header_count > SIZE_MAX - 2u)
    return SALTS_ERANGE;
  stream->owner = owner;
  stream->response_header_capacity = config->max_response_header_count + 2u;
  stream->headers = (chttp_header *)calloc(config->max_header_count, sizeof(*stream->headers));
  stream->response_headers = (chttp_h2_hpack_header *)calloc(stream->response_header_capacity,
                                                             sizeof(*stream->response_headers));
  stream->header_storage = (char *)malloc(stream->header_storage_capacity);
  stream->target_storage = (char *)malloc(target_stride * 2u);
  stream->authority = (char *)malloc(config->max_header_bytes + 1u);
  if (stream->headers == NULL || stream->response_headers == NULL ||
      stream->header_storage == NULL || stream->target_storage == NULL ||
      stream->authority == NULL) {
    chttp_h2_server_stream_destroy(stream);
    return SALTS_ENOMEM;
  }
  status = chttp_server_request_state_init(&stream->request_state, owner->connection->server);
  if (status != SALTS_OK) {
    chttp_h2_server_stream_destroy(stream);
    return status;
  }
  stream->deferred_builder.server = owner->connection->server;
  status = chttp_server_response_builder_init(&stream->deferred_builder, config);
  if (status != SALTS_OK) {
    chttp_h2_server_stream_destroy(stream);
    return status;
  }
  stream->deferred_response.impl = &stream->deferred_builder;
  atomic_init(&stream->deferred_token, chttp_server_deferred_token(0u, CHTTP_SERVER_DEFERRED_IDLE));
  stream->deferred_target =
      (chttp_server_deferred_target){.server = owner->connection->server,
                                     .connection = owner->connection,
                                     .request_state = &stream->request_state,
                                     .response_builder = &stream->deferred_builder,
                                     .response = &stream->deferred_response,
                                     .token = &stream->deferred_token,
                                     .kind = CHTTP_SERVER_DEFERRED_HTTP_2};
  stream->request_state.response_builder.connection = owner->connection;
  stream->request_state.response_builder.deferred_target = &stream->deferred_target;
  chttp_h2_server_stream_reset(stream);
  stream->owner = owner;
  return SALTS_OK;
}

static chttp_h2_server_stream *chttp_h2_server_stream_find(chttp_h2_server_connection *h2,
                                                           int32_t stream_id) {
  size_t index;
  if (h2 == NULL) return NULL;
  for (index = 0u; index < h2->stream_capacity; ++index)
    if (h2->streams[index].active && !h2->streams[index].protocol_closed &&
        h2->streams[index].stream_id == stream_id)
      return &h2->streams[index];
  return NULL;
}

chttp_server_websocket_peer *chttp_h2_server_websocket_peer_find(chttp_h2_server_connection *h2,
                                                                 int32_t stream_id) {
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  if (stream == NULL || stream->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE) return NULL;
  return &stream->websocket_peer;
}

static chttp_h2_server_stream *chttp_h2_server_stream_acquire(chttp_h2_server_connection *h2,
                                                              int32_t stream_id) {
  size_t index;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_h2_server_stream *stream = &h2->streams[index];
    if (stream->active) continue;
    chttp_h2_server_stream_reset(stream);
    stream->owner = h2;
    stream->active = true;
    stream->stream_id = stream_id;
    ++h2->active_streams;
    return stream;
  }
  return NULL;
}

static int chttp_h2_server_copy(char *output, size_t capacity, const char *input, size_t size) {
  if (output == NULL || input == NULL || size >= capacity) return SALTS_EMSGSIZE;
  memcpy(output, input, size);
  output[size] = '\0';
  return SALTS_OK;
}

static int chttp_h2_server_target(chttp_h2_server_stream *stream, const char *value, size_t size) {
  const chttp_server_config *config = &stream->owner->connection->server->config;
  const char *query;
  char *path;
  size_t path_size;
  size_t index;
  const size_t stride = config->max_target_bytes + 1u;
  if (size == 0u || size > config->max_target_bytes ||
      (value[0] != '/' &&
       !(stream->method == CHTTP_METHOD_OPTIONS && size == 1u && value[0] == '*')))
    return SALTS_EPROTO;
  for (index = 0u; index < size; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if (byte <= 0x20u || byte >= 0x7fu || byte == (unsigned char)'#') return SALTS_EPROTO;
  }
  if (chttp_h2_server_copy(stream->target_storage, stride, value, size) != SALTS_OK)
    return SALTS_EMSGSIZE;
  query = memchr(value, '?', size);
  path_size = query == NULL ? size : (size_t)(query - value);
  if (path_size == 0u) return SALTS_EPROTO;
  path = stream->target_storage + stride;
  return chttp_h2_server_copy(path, stride, value, path_size);
}

static int chttp_h2_server_regular_header(chttp_h2_server_stream *stream, const char *name,
                                          size_t name_size, const char *value, size_t value_size) {
  const chttp_server_config *config = &stream->owner->connection->server->config;
  char *name_copy;
  char *value_copy;
  size_t needed;
  size_t index;
  if (stream->header_count >= config->max_header_count ||
      !chttp_h2_server_add(name_size, value_size, &needed) ||
      !chttp_h2_server_add(needed, 2u, &needed) ||
      stream->header_storage_used > stream->header_storage_capacity ||
      needed > stream->header_storage_capacity - stream->header_storage_used ||
      !chttp_h2_server_header_value(value, value_size) ||
      chttp_h2_server_forbidden_header(name, name_size))
    return SALTS_EPROTO;
  for (index = 0u; index < name_size; ++index)
    if (!chttp_h2_server_header_name_byte((unsigned char)name[index])) return SALTS_EPROTO;
  if (chttp_h2_server_ascii_equal_n(name, name_size, "te") &&
      !chttp_h2_server_ascii_equal_n(value, value_size, "trailers"))
    return SALTS_EPROTO;
  if (stream->extended_connect) {
    if (chttp_h2_server_ascii_equal_n(name, name_size, "sec-websocket-version")) {
      if (stream->websocket_version_seen || !chttp_h2_server_ascii_equal_n(value, value_size, "13"))
        return SALTS_EPROTO;
      stream->websocket_version_seen = true;
    } else if (chttp_h2_server_ascii_equal_n(name, name_size, "sec-websocket-key") ||
               chttp_h2_server_ascii_equal_n(name, name_size, "sec-websocket-accept") ||
               chttp_h2_server_ascii_equal_n(name, name_size, "sec-websocket-extensions") ||
               chttp_h2_server_ascii_equal_n(name, name_size, "content-length") ||
               chttp_h2_server_ascii_equal_n(name, name_size, "te")) {
      return SALTS_EPROTO;
    }
  }
  if (chttp_h2_server_ascii_equal_n(name, name_size, "content-length")) {
    size_t content_length;
    if (stream->trailers || stream->content_length_seen ||
        chttp_h2_server_decimal(value, value_size, &content_length) != SALTS_OK)
      return SALTS_EPROTO;
    stream->content_length_seen = true;
    stream->content_length = content_length;
  }
  name_copy = stream->header_storage + stream->header_storage_used;
  memcpy(name_copy, name, name_size);
  name_copy[name_size] = '\0';
  value_copy = name_copy + name_size + 1u;
  memcpy(value_copy, value, value_size);
  value_copy[value_size] = '\0';
  stream->headers[stream->header_count++] = (chttp_header){name_copy, value_copy};
  stream->header_storage_used += needed;
  return SALTS_OK;
}

static int chttp_h2_server_begin_headers(void *user, int32_t stream_id) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  if (h2 == NULL) return -1;
  if (stream == NULL) {
    if (h2->draining) return -1;
    stream = chttp_h2_server_stream_acquire(h2, stream_id);
    if (stream == NULL || chttp_h2_proto_set_stream_user_data(h2->protocol, stream_id, stream) != 0)
      return -1;
  } else {
    if (stream->header_block_open || stream->response_submitted) return -1;
    stream->trailers = true;
  }
  stream->header_block_open = true;
  stream->regular_header_seen = false;
  return 0;
}

static int chttp_h2_server_header(void *user, int32_t stream_id, const char *name, size_t name_size,
                                  const char *value, size_t value_size) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  size_t byte_index;
  if (stream == NULL || !stream->header_block_open || name == NULL || name_size == 0u ||
      value == NULL)
    return -1;
  for (byte_index = 0u; byte_index < name_size; ++byte_index)
    if (name[byte_index] >= 'A' && name[byte_index] <= 'Z') return -1;
  if (name[0] != ':') {
    if (stream->trailers) return -1;
    stream->regular_header_seen = true;
    return chttp_h2_server_regular_header(stream, name, name_size, value, value_size) == SALTS_OK
               ? 0
               : -1;
  }
  if (stream->trailers || stream->regular_header_seen) return -1;
  if (chttp_h2_server_ascii_equal_n(name, name_size, ":method")) {
    if (stream->method_seen) return -1;
    stream->method = chttp_h2_server_method(value, value_size, &stream->extended_connect);
    if ((int)stream->method == 0) return -1;
    stream->method_seen = true;
    return 0;
  }
  if (chttp_h2_server_ascii_equal_n(name, name_size, ":scheme")) {
    const bool tls = stream->owner->connection->server->tls_initialized;
    if (stream->scheme_seen ||
        !(chttp_h2_server_ascii_equal_n(value, value_size, tls ? "https" : "http")))
      return -1;
    stream->scheme_seen = true;
    return 0;
  }
  if (chttp_h2_server_ascii_equal_n(name, name_size, ":path")) {
    if (stream->path_seen || chttp_h2_server_target(stream, value, value_size) != SALTS_OK)
      return -1;
    stream->path_seen = true;
    return 0;
  }
  if (chttp_h2_server_ascii_equal_n(name, name_size, ":authority")) {
    size_t index;
    const size_t capacity = stream->owner->connection->server->config.max_header_bytes + 1u;
    if (stream->authority_seen || value_size == 0u || value_size >= capacity) return -1;
    for (index = 0u; index < value_size; ++index) {
      const unsigned char byte = (unsigned char)value[index];
      if (byte <= 0x20u || byte >= 0x7fu || byte == (unsigned char)'/' ||
          byte == (unsigned char)'?' || byte == (unsigned char)'#' || byte == (unsigned char)'@')
        return -1;
    }
    if (chttp_h2_server_copy(stream->authority, capacity, value, value_size) != SALTS_OK) return -1;
    stream->authority_seen = true;
    return 0;
  }
  if (chttp_h2_server_ascii_equal_n(name, name_size, ":protocol")) {
    if (stream->protocol_seen || !chttp_h2_server_ascii_equal_n(value, value_size, "websocket"))
      return -1;
    stream->protocol_seen = true;
    return 0;
  }
  return -1;
}

static bool chttp_h2_server_response_header_forbidden(const char *name) {
  return chttp_h2_server_ascii_equal_n(name, strlen(name), "connection") ||
         chttp_h2_server_ascii_equal_n(name, strlen(name), "proxy-connection") ||
         chttp_h2_server_ascii_equal_n(name, strlen(name), "keep-alive") ||
         chttp_h2_server_ascii_equal_n(name, strlen(name), "transfer-encoding") ||
         chttp_h2_server_ascii_equal_n(name, strlen(name), "upgrade");
}

static void chttp_h2_server_file_ready(void *user) {
  chttp_h2_server_stream *stream = (chttp_h2_server_stream *)user;
  chttp_server_response_builder *builder;
  chttp_server_connection *connection;
  int status;
  if (stream == NULL || !stream->active || !stream->response_submitted || stream->owner == NULL ||
      stream->owner->protocol == NULL)
    return;
  builder = &stream->request_state.response_builder;
  if (builder->file_transfer == NULL || !chttp_file_transfer_ready(builder->file_transfer)) return;
  connection = stream->owner->connection;
  if (chttp_h2_proto_resume_source(stream->owner->protocol, stream->stream_id) != 0) {
    chttp_server_response_builder_close_source(builder, SALTS_EPROTO);
    chttp_server_connection_close(connection);
    return;
  }
  status = chttp_h2_server_connection_flush(stream->owner);
  if (status != SALTS_OK) {
    chttp_server_response_builder_close_source(builder, status);
    chttp_server_connection_close(connection);
    return;
  }
  (void)chttp_server_send_pending(connection);
}

static chttp_h2_proto_source_result
chttp_h2_server_response_source_read(void *user, uint8_t *buffer, size_t capacity) {
  chttp_h2_server_stream *stream = (chttp_h2_server_stream *)user;
  chttp_server_response_builder *builder;
  chttp_body_source *source;
  size_t remaining;
  size_t size = 0u;
  int status;
  if (stream == NULL || buffer == NULL || capacity == 0u)
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  builder = &stream->request_state.response_builder;
  if (!builder->source_enabled || builder->body_source.read == NULL)
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  source = &builder->body_source;
  if (source->content_length_known) {
    if (builder->source_transferred > source->content_length) {
      chttp_server_response_builder_close_source(builder, SALTS_EPROTO);
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
    remaining = source->content_length - builder->source_transferred;
    if (remaining != 0u && capacity > remaining) capacity = remaining;
  } else {
    if (builder->source_transferred > builder->source_capacity) {
      chttp_server_response_builder_close_source(builder, SALTS_EMSGSIZE);
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
    remaining = builder->source_capacity - builder->source_transferred;
    if (remaining != 0u && capacity > remaining) capacity = remaining;
  }
  if (builder->file_transfer != NULL) {
    const chttp_file_source_result result =
        chttp_file_transfer_read(builder->file_transfer, buffer, capacity, &size);
    if (result == CHTTP_FILE_SOURCE_WAIT)
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_WAIT, 0u};
    if (result == CHTTP_FILE_SOURCE_ERROR) {
      status = chttp_file_transfer_status(builder->file_transfer, NULL);
      chttp_server_response_builder_close_source(builder, status == SALTS_OK ? SALTS_EIO : status);
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
    if (result == CHTTP_FILE_SOURCE_EOF) size = 0u;
  } else {
    status = source->read(source->user, buffer, capacity, &size);
    if (status != SALTS_OK) {
      chttp_server_response_builder_close_source(builder, status);
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
  }
  if (size > capacity || (remaining == 0u && size != 0u)) {
    chttp_server_response_builder_close_source(
        builder, source->content_length_known ? SALTS_EPROTO : SALTS_EMSGSIZE);
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  }
  if (source->content_length_known && remaining != 0u && size == 0u) {
    chttp_server_response_builder_close_source(builder, SALTS_EPROTO);
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  }
  if (size == 0u) {
    chttp_server_response_builder_close_source(builder, SALTS_OK);
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_EOF, 0u};
  }
  builder->source_transferred += size;
  return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_DATA, size};
}

static int chttp_h2_server_submit_response_builder(chttp_h2_server_stream *stream,
                                                   chttp_server_response_builder *builder) {
  chttp_server_impl *server = stream->owner->connection->server;
  const bool source_response = builder->source_enabled;
  const bool stream_source = source_response && stream->method != CHTTP_METHOD_HEAD;
  const size_t body_size =
      stream->method == CHTTP_METHOD_HEAD || stream_source ? 0u : builder->body_size;
  size_t header_count = 0u;
  size_t index;
  char status_text[4];
  char content_length[3u * sizeof(size_t) + 1u];
  int status_size;
  int length_size = 0;
  int submit_status;
  status_size = snprintf(status_text, sizeof(status_text), "%u", builder->status_code);
  if (status_size != 3) return SALTS_EPROTO;
  stream->response_headers[header_count++] =
      (chttp_h2_hpack_header){":status", sizeof(":status") - 1u, status_text, (size_t)status_size};
  for (index = 0u; index < builder->header_count; ++index) {
    chttp_header *header = &builder->headers[index];
    char *name = (char *)header->name;
    size_t name_size;
    size_t byte_index;
    if (chttp_h2_server_response_header_forbidden(header->name)) continue;
    name_size = strlen(name);
    for (byte_index = 0u; byte_index < name_size; ++byte_index)
      name[byte_index] = (char)chttp_h2_server_ascii_lower((unsigned char)name[byte_index]);
    if (header_count >= stream->response_header_capacity) return SALTS_ENOBUFS;
    stream->response_headers[header_count++] =
        (chttp_h2_hpack_header){header->name, name_size, header->value, strlen(header->value)};
  }
  if (builder->status_code != 204u && builder->status_code != 304u &&
      (!source_response || builder->body_source.content_length_known)) {
    length_size = snprintf(content_length, sizeof(content_length), "%zu", builder->body_size);
    if (length_size <= 0 || (size_t)length_size >= sizeof(content_length) ||
        header_count >= stream->response_header_capacity)
      return SALTS_EMSGSIZE;
    stream->response_headers[header_count++] = (chttp_h2_hpack_header){
        "content-length", sizeof("content-length") - 1u, content_length, (size_t)length_size};
  }
  if (source_response && !stream_source)
    chttp_server_response_builder_close_source(builder, SALTS_OK);
  if (stream_source && builder->file_transfer != NULL)
    chttp_file_transfer_set_ready(builder->file_transfer, chttp_h2_server_file_ready, stream);
  stream->response_submitted = true;
  submit_status = chttp_h2_proto_submit_response_ex(
      stream->owner->protocol, stream->stream_id, stream->response_headers, header_count,
      body_size == 0u ? NULL : builder->body, body_size,
      stream_source ? chttp_h2_server_response_source_read : NULL, stream_source ? stream : NULL);
  if (submit_status != 0) {
    stream->response_submitted = false;
    chttp_server_response_builder_close_source(builder, SALTS_ENOBUFS);
    chttp_session_request_abort(&stream->request_state);
    return SALTS_ENOBUFS;
  }
  chttp_server_stats_response(server);
  return SALTS_OK;
}

static int chttp_h2_server_submit_response(chttp_h2_server_stream *stream) {
  return chttp_h2_server_submit_response_builder(stream, &stream->request_state.response_builder);
}

static chttp_server_request_view
chttp_h2_server_request_view(const chttp_h2_server_stream *stream) {
  const chttp_server_config *config = &stream->owner->connection->server->config;
  const size_t stride = config->max_target_bytes + 1u;
  const bool streamed = chttp_server_request_body_streaming(&stream->request_state);
  chttp_server_request_view request = {.http_major = 2u,
                                       .http_minor = 0u,
                                       .method = stream->method,
                                       .target = stream->target_storage,
                                       .path = stream->target_storage + stride,
                                       .headers = stream->headers,
                                       .header_count = stream->header_count,
                                       .body = streamed ? NULL : stream->body,
                                       .body_size = stream->body_size,
                                       .body_streamed = streamed ? 1 : 0,
                                       .protocol_keep_alive = 1};
  chttp_server_request_enrich(stream->owner->connection, &request);
  return request;
}

static int chttp_h2_server_websocket_write(void *transport, const uint8_t *data, size_t size) {
  chttp_h2_server_stream *stream = (chttp_h2_server_stream *)transport;
  if (stream == NULL || !stream->active || data == NULL || size == 0u ||
      stream->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE)
    return SALTS_EINVAL;
  if (stream->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_HANDSHAKE ||
      stream->websocket_output_size != 0u ||
      chttp_h2_proto_stream_output_pending(stream->owner->protocol, stream->stream_id))
    return SALTS_EBUSY;
  if (size > stream->owner->connection->server->config.network.max_send_bytes)
    return SALTS_EMSGSIZE;
  {
    const int status = chttp_server_buffer_grow(
        stream->owner->connection->server, &stream->websocket_output,
        &stream->websocket_output_capacity, size,
        stream->owner->connection->server->config.network.max_send_bytes, 0u);
    if (status != SALTS_OK) return status;
  }
  memcpy(stream->websocket_output, data, size);
  stream->websocket_output_size = size;
  if (chttp_h2_proto_submit_data(stream->owner->protocol, stream->stream_id,
                                 stream->websocket_output, size, 0) != 0) {
    stream->websocket_output_size = 0u;
    chttp_server_buffer_release(stream->owner->connection->server, stream->websocket_output,
                                stream->websocket_output_capacity);
    stream->websocket_output = NULL;
    stream->websocket_output_capacity = 0u;
    return SALTS_ENOBUFS;
  }
  return SALTS_OK;
}

static int chttp_h2_server_websocket_status(chttp_h2_server_stream *stream,
                                            unsigned int status_code) {
  char status_text[4];
  chttp_h2_hpack_header header;
  const int size = snprintf(status_text, sizeof(status_text), "%u", status_code);
  if (size != 3) return SALTS_EINVAL;
  header = (chttp_h2_hpack_header){":status", sizeof(":status") - 1u, status_text, (size_t)size};
  stream->response_submitted = true;
  if (chttp_h2_proto_submit_response(stream->owner->protocol, stream->stream_id, &header, 1u, NULL,
                                     0u) != 0) {
    stream->response_submitted = false;
    return SALTS_ENOBUFS;
  }
  chttp_server_stats_response(stream->owner->connection->server);
  return SALTS_OK;
}

static int chttp_h2_server_websocket_accept(chttp_h2_server_stream *stream) {
  chttp_server_response_builder *builder = &stream->request_state.response_builder;
  size_t header_count = 0u;
  size_t index;
  static const char status[] = "200";
  stream->response_headers[header_count++] =
      (chttp_h2_hpack_header){":status", sizeof(":status") - 1u, status, sizeof(status) - 1u};
  for (index = 0u; index < builder->header_count; ++index) {
    chttp_header *header = &builder->headers[index];
    char *name = (char *)header->name;
    size_t name_size = strlen(name);
    size_t byte_index;
    if (chttp_h2_server_response_header_forbidden(name)) continue;
    if (header_count >= stream->response_header_capacity) return SALTS_ENOBUFS;
    for (byte_index = 0u; byte_index < name_size; ++byte_index)
      name[byte_index] = (char)chttp_h2_server_ascii_lower((unsigned char)name[byte_index]);
    stream->response_headers[header_count++] =
        (chttp_h2_hpack_header){name, name_size, header->value, strlen(header->value)};
  }
  stream->response_submitted = true;
  if (chttp_h2_proto_submit_headers(stream->owner->protocol, stream->stream_id,
                                    stream->response_headers, header_count, 0) != 0) {
    stream->response_submitted = false;
    return SALTS_ENOBUFS;
  }
  chttp_server_stats_response(stream->owner->connection->server);
  chttp_server_websocket_peer_open(&stream->websocket_peer);
  return chttp_server_websocket_peer_flush(&stream->websocket_peer);
}

static int chttp_h2_server_websocket_dispatch(chttp_h2_server_stream *stream) {
  chttp_server_request_view request;
  chttp_server_route_record *route;
  int status;
  if (!stream->extended_connect || !stream->protocol_seen || !stream->websocket_version_seen ||
      stream->content_length_seen || stream->body_size != 0u)
    return SALTS_EPROTO;
  request = chttp_h2_server_request_view(stream);
  status = chttp_server_request_admit(&stream->request_state, &request, CHTTP_METHOD_GET);
  if (status == SALTS_EPERM) {
    chttp_server_response_builder_reset(&stream->request_state.response_builder);
    status = chttp_jwt_bearer_unauthorized_response(&stream->request_state.response);
    if (status == SALTS_OK) status = chttp_h2_server_submit_response(stream);
    return status;
  }
  if (status != SALTS_OK) return status;
  route = stream->request_state.admitted_route;
  if (route == NULL || !route->websocket) return chttp_h2_server_websocket_status(stream, 404u);
  status =
      chttp_server_websocket_peer_init(&stream->websocket_peer, stream->owner->connection->server,
                                       route, stream->owner->connection->handle, stream->stream_id,
                                       chttp_h2_server_websocket_write, stream);
  if (status != SALTS_OK) return status;
  status = chttp_server_websocket_route_open(&stream->websocket_peer, &stream->request_state, route,
                                             &request);
  if (status != SALTS_OK) {
    chttp_server_websocket_peer_reset(&stream->websocket_peer);
    return chttp_h2_server_websocket_status(stream, 500u);
  }
  if (stream->request_state.response_builder.replied) {
    if (stream->request_state.response_builder.source_enabled) {
      chttp_server_response_builder_close_source(&stream->request_state.response_builder,
                                                 SALTS_ENOTSUP);
      chttp_server_websocket_peer_reset(&stream->websocket_peer);
      return chttp_h2_server_websocket_status(stream, 500u);
    }
    chttp_server_websocket_peer_reset(&stream->websocket_peer);
    return chttp_h2_server_submit_response(stream);
  }
  return chttp_h2_server_websocket_accept(stream);
}

static int chttp_h2_server_dispatch(chttp_h2_server_stream *stream) {
  chttp_server_request_view request;
  chttp_server_response_builder *builder = &stream->request_state.response_builder;
  int status;
  if (stream->response_submitted || !stream->method_seen || !stream->scheme_seen ||
      !stream->path_seen || !stream->authority_seen || stream->extended_connect ||
      stream->protocol_seen ||
      (stream->target_storage[0] == '*' && stream->method != CHTTP_METHOD_OPTIONS) ||
      (stream->content_length_seen && stream->content_length != stream->body_size)) {
    chttp_server_request_body_close(&stream->request_state, SALTS_EPROTO);
    return SALTS_EPROTO;
  }
  chttp_server_request_body_close(&stream->request_state, SALTS_OK);
  request = chttp_h2_server_request_view(stream);
  builder->request = &request;
  status = chttp_server_dispatch_request(&stream->request_state, &request);
  builder->request = NULL;
  if (status != SALTS_OK) return status;
  if (builder->deferred) return SALTS_OK;
  return chttp_h2_server_submit_response(stream);
}

static int chttp_h2_server_dispatch_or_reset(chttp_h2_server_stream *stream) {
  chttp_h2_proto *protocol = stream->owner->protocol;
  const int32_t stream_id = stream->stream_id;
  const int status = chttp_h2_server_dispatch(stream);
  const uint32_t error_code =
      status == SALTS_EPROTO ? CHTTP_H2_ERR_PROTOCOL_ERROR : CHTTP_H2_ERR_INTERNAL_ERROR;
  if (status == SALTS_OK) return 0;
  /* submit_rst_stream synchronously releases the application stream, so only
   * the stable protocol and stream id captured above may be used afterwards. */
  return chttp_h2_proto_submit_rst_stream(protocol, stream_id, error_code) == 0 ? 0 : -1;
}

static int chttp_h2_server_end_headers(void *user, int32_t stream_id, int end_stream) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  chttp_server_request_view request;
  chttp_body_sink sink = {0};
  int status;
  if (stream == NULL || !stream->header_block_open) return -1;
  stream->header_block_open = false;
  if (stream->trailers && !end_stream) return -1;
  if (!stream->trailers && (!stream->method_seen || !stream->scheme_seen || !stream->path_seen ||
                            !stream->authority_seen))
    return -1;
  if (!stream->trailers && (stream->extended_connect || stream->protocol_seen)) {
    if (end_stream) return -1;
    status = chttp_h2_server_websocket_dispatch(stream);
    if (status == SALTS_OK) return 0;
    return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                            status == SALTS_EPROTO
                                                ? CHTTP_H2_ERR_PROTOCOL_ERROR
                                                : CHTTP_H2_ERR_INTERNAL_ERROR) == 0
               ? 0
               : -1;
  }
  if (!stream->trailers) {
    request = chttp_h2_server_request_view(stream);
    status = chttp_server_request_admit(&stream->request_state, &request, request.method);
    if (status == SALTS_EPERM) {
      chttp_server_response_builder_reset(&stream->request_state.response_builder);
      status = chttp_jwt_bearer_unauthorized_response(&stream->request_state.response);
      if (status == SALTS_OK) status = chttp_h2_server_submit_response(stream);
      return status == SALTS_OK ? 0 : -1;
    }
    if (status != SALTS_OK) {
      chttp_server_request_body_close(&stream->request_state, status);
      return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                              CHTTP_H2_ERR_INTERNAL_ERROR) == 0
                 ? 0
                 : -1;
    }
    status = chttp_server_request_body_open(&stream->request_state, &request, &sink);
    if (status != SALTS_OK) {
      chttp_server_request_body_close(&stream->request_state, status);
      return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                              CHTTP_H2_ERR_INTERNAL_ERROR) == 0
                 ? 0
                 : -1;
    }
  }
  if (end_stream) return chttp_h2_server_dispatch_or_reset(stream);
  return stream->trailers ? -1 : 0;
}

static int chttp_h2_server_data(void *user, int32_t stream_id, const uint8_t *data, size_t size) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  const size_t capacity = h2->connection->server->config.max_request_body_bytes;
  if (stream == NULL || stream->header_block_open || stream->trailers ||
      (size != 0u && data == NULL))
    return -1;
  if (stream->websocket_peer.phase != CHTTP_SERVER_WEBSOCKET_NONE) {
    int websocket_status = SALTS_OK;
    if (size != 0u)
      websocket_status = chttp_server_websocket_peer_feed(&stream->websocket_peer, data, size);
    if (size != 0u && (chttp_h2_proto_consume_stream(h2->protocol, stream_id, size) != 0 ||
                       chttp_h2_proto_consume_connection(h2->protocol, size) != 0))
      return -1;
    if (websocket_status != SALTS_OK) {
      chttp_server_stats_protocol_error(h2->connection->server);
      return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                              CHTTP_H2_ERR_PROTOCOL_ERROR) == 0
                 ? 0
                 : -1;
    }
    if (chttp_h2_proto_remote_end_stream(h2->protocol, stream_id))
      chttp_server_websocket_peer_transport_closed(&stream->websocket_peer);
    return 0;
  }
  if (stream->response_submitted && stream->request_state.admission_rejected) {
    if (size != 0u && (chttp_h2_proto_consume_stream(h2->protocol, stream_id, size) != 0 ||
                       chttp_h2_proto_consume_connection(h2->protocol, size) != 0))
      return -1;
    return 0;
  }
  if (stream->response_submitted) return -1;
  if (stream->body_size > capacity || size > capacity - stream->body_size) {
    if (size != 0u && chttp_h2_proto_consume_connection(h2->protocol, size) != 0) return -1;
    chttp_server_request_body_close(&stream->request_state, SALTS_EMSGSIZE);
    return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                            CHTTP_H2_ERR_ENHANCE_YOUR_CALM) == 0
               ? 0
               : -1;
  }
  if (size != 0u) {
    if (chttp_server_request_body_streaming(&stream->request_state)) {
      const int sink_status = chttp_server_request_body_write(&stream->request_state, data, size);
      if (sink_status != SALTS_OK) {
        if (chttp_h2_proto_consume_connection(h2->protocol, size) != 0) return -1;
        chttp_server_request_body_close(&stream->request_state, sink_status);
        return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                                CHTTP_H2_ERR_INTERNAL_ERROR) == 0
                   ? 0
                   : -1;
      }
    } else {
      const int grow_status =
          chttp_server_buffer_grow(h2->connection->server, &stream->body, &stream->body_capacity,
                                   stream->body_size + size, capacity, stream->body_size);
      if (grow_status != SALTS_OK) {
        if (chttp_h2_proto_consume_connection(h2->protocol, size) != 0) return -1;
        chttp_server_request_body_close(&stream->request_state, grow_status);
        return chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id,
                                                CHTTP_H2_ERR_INTERNAL_ERROR) == 0
                   ? 0
                   : -1;
      }
      memcpy(stream->body + stream->body_size, data, size);
    }
    stream->body_size += size;
    if (chttp_h2_proto_consume_stream(h2->protocol, stream_id, size) != 0 ||
        chttp_h2_proto_consume_connection(h2->protocol, size) != 0)
      return -1;
  }
  if (chttp_h2_proto_remote_end_stream(h2->protocol, stream_id))
    return chttp_h2_server_dispatch_or_reset(stream);
  return 0;
}

static int chttp_h2_server_stream_close(void *user, int32_t stream_id, uint32_t error_code) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  chttp_h2_server_stream *stream = chttp_h2_server_stream_find(h2, stream_id);
  uint_fast64_t token;
  (void)error_code;
  if (stream == NULL) return 0;
  (void)chttp_h2_proto_set_stream_user_data(h2->protocol, stream_id, NULL);
  if (h2->active_streams != 0u) --h2->active_streams;
  stream->protocol_closed = true;
  token = atomic_load_explicit(&stream->deferred_token, memory_order_acquire);
  for (;;) {
    const chttp_server_deferred_state state = chttp_server_deferred_token_state(token);
    if (state == CHTTP_SERVER_DEFERRED_WRITING) return 0;
    if (state == CHTTP_SERVER_DEFERRED_IDLE ||
        atomic_compare_exchange_weak_explicit(
            &stream->deferred_token, &token,
            chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                                        CHTTP_SERVER_DEFERRED_IDLE),
            memory_order_acq_rel, memory_order_acquire))
      break;
  }
  chttp_h2_server_stream_reset(stream);
  stream->owner = h2;
  return 0;
}

static void chttp_h2_server_ping_ack(void *user, const uint8_t opaque[8]) {
  chttp_h2_server_connection *h2 = (chttp_h2_server_connection *)user;
  if (h2 != NULL && h2->drain_ping_sent &&
      memcmp(opaque, CHTTP_H2_SERVER_DRAIN_PING, sizeof(CHTTP_H2_SERVER_DRAIN_PING)) == 0)
    h2->drain_ping_acked = true;
}

int chttp_h2_server_connection_init(chttp_h2_server_connection **out_h2,
                                    chttp_server_connection *connection) {
  chttp_h2_server_connection *h2;
  size_t index;
  int status;
  if (out_h2 == NULL || connection == NULL || connection->server == NULL ||
      !connection->server->config.enable_http2)
    return SALTS_EINVAL;
  *out_h2 = NULL;
  h2 = (chttp_h2_server_connection *)calloc(1u, sizeof(*h2));
  if (h2 == NULL) return SALTS_ENOMEM;
  h2->connection = connection;
  h2->stream_capacity = connection->server->config.h2_stream_capacity;
  status = chttp_h2_server_config_validate(&connection->server->config, &h2->protocol_config);
  if (status != SALTS_OK) {
    free(h2);
    return status;
  }
  h2->streams = (chttp_h2_server_stream *)calloc(h2->stream_capacity, sizeof(*h2->streams));
  if (h2->streams == NULL) {
    free(h2);
    return SALTS_ENOMEM;
  }
  for (index = 0u; index < h2->stream_capacity; ++index) {
    status = chttp_h2_server_stream_init(&h2->streams[index], h2);
    if (status != SALTS_OK) {
      chttp_h2_server_connection_destroy(h2);
      return status;
    }
  }
  *out_h2 = h2;
  return SALTS_OK;
}

int chttp_h2_server_connection_prepare(chttp_h2_server_connection *h2) {
  chttp_h2_proto_callbacks callbacks = {0};
  size_t index;
  if (h2 == NULL || h2->connection == NULL) return SALTS_EINVAL;
  if (chttp_h2_server_connection_has_deferred(h2)) return SALTS_EBUSY;
  chttp_h2_proto_destroy(h2->protocol);
  h2->protocol = NULL;
  h2->active_streams = 0u;
  h2->draining = false;
  h2->drain_ping_sent = false;
  h2->drain_ping_acked = false;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_h2_server_stream_reset(&h2->streams[index]);
    h2->streams[index].owner = h2;
  }
  callbacks.user_data = h2;
  callbacks.on_begin_headers = chttp_h2_server_begin_headers;
  callbacks.on_header = chttp_h2_server_header;
  callbacks.on_end_headers = chttp_h2_server_end_headers;
  callbacks.on_data = chttp_h2_server_data;
  callbacks.on_stream_close = chttp_h2_server_stream_close;
  callbacks.on_ping_ack = chttp_h2_server_ping_ack;
  h2->protocol = chttp_h2_proto_create(CHTTP_H2_PROTO_SERVER, &h2->protocol_config, &callbacks);
  if (h2->protocol != NULL) {
    chttp_h2_proto_set_local_enable_connect_protocol(h2->protocol, 1u);
    chttp_h2_proto_set_send_chunk(h2->protocol, h2->connection->server->config.stream_chunk_bytes);
  }
  return h2->protocol == NULL ? SALTS_ENOMEM : SALTS_OK;
}

void chttp_h2_server_connection_release(chttp_h2_server_connection *h2) {
  size_t index;
  if (h2 == NULL) return;
  chttp_h2_proto_destroy(h2->protocol);
  h2->protocol = NULL;
  h2->active_streams = 0u;
  h2->draining = false;
  h2->drain_ping_sent = false;
  h2->drain_ping_acked = false;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_h2_server_stream *stream = &h2->streams[index];
    uint_fast64_t token = atomic_load_explicit(&stream->deferred_token, memory_order_acquire);
    stream->protocol_closed = true;
    for (;;) {
      const chttp_server_deferred_state state = chttp_server_deferred_token_state(token);
      if (state == CHTTP_SERVER_DEFERRED_WRITING) break;
      if (state == CHTTP_SERVER_DEFERRED_IDLE ||
          atomic_compare_exchange_weak_explicit(
              &stream->deferred_token, &token,
              chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                                          CHTTP_SERVER_DEFERRED_IDLE),
              memory_order_acq_rel, memory_order_acquire)) {
        chttp_h2_server_stream_reset(stream);
        stream->owner = h2;
        break;
      }
    }
  }
}

void chttp_h2_server_connection_destroy(chttp_h2_server_connection *h2) {
  size_t index;
  if (h2 == NULL) return;
  chttp_h2_proto_destroy(h2->protocol);
  if (h2->streams != NULL)
    for (index = 0u; index < h2->stream_capacity; ++index)
      chttp_h2_server_stream_destroy(&h2->streams[index]);
  free(h2->streams);
  free(h2);
}

void chttp_h2_server_connection_cancel_file_sources(chttp_h2_server_connection *h2) {
  size_t index;
  if (h2 == NULL || h2->streams == NULL) return;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_server_response_builder *builder = &h2->streams[index].request_state.response_builder;
    if (builder->file_transfer != NULL)
      chttp_server_response_builder_close_source(builder, SALTS_ECANCELED);
  }
}

int chttp_h2_server_connection_receive(chttp_h2_server_connection *h2, const void *data,
                                       size_t size) {
  ptrdiff_t consumed;
  if (h2 == NULL || h2->protocol == NULL || (size != 0u && data == NULL)) return SALTS_EINVAL;
  consumed = chttp_h2_proto_recv(h2->protocol, (const uint8_t *)data, size);
  return consumed >= 0 && (size_t)consumed == size ? SALTS_OK : SALTS_EPROTO;
}

static int chttp_h2_server_websockets_drive(chttp_h2_server_connection *h2) {
  size_t index;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_h2_server_stream *stream = &h2->streams[index];
    int status;
    if (!stream->active || stream->websocket_peer.phase == CHTTP_SERVER_WEBSOCKET_NONE) continue;
    if (stream->websocket_output_size != 0u &&
        !chttp_h2_proto_stream_output_pending(h2->protocol, stream->stream_id)) {
      stream->websocket_output_size = 0u;
      chttp_server_buffer_release(h2->connection->server, stream->websocket_output,
                                  stream->websocket_output_capacity);
      stream->websocket_output = NULL;
      stream->websocket_output_capacity = 0u;
    }
    if (stream->websocket_output_size == 0u && !stream->websocket_end_submitted) {
      status = chttp_server_websocket_peer_flush(&stream->websocket_peer);
      if (status != SALTS_OK) return status;
    }
    if (!stream->websocket_end_submitted && stream->websocket_output_size == 0u &&
        !chttp_h2_proto_stream_output_pending(h2->protocol, stream->stream_id) &&
        chttp_server_websocket_peer_terminal(&stream->websocket_peer)) {
      if (chttp_h2_proto_submit_data(h2->protocol, stream->stream_id, NULL, 0u, 1) != 0)
        return SALTS_ENOBUFS;
      stream->websocket_end_submitted = true;
    }
  }
  return SALTS_OK;
}

bool chttp_h2_server_connection_has_deferred(const chttp_h2_server_connection *h2) {
  size_t index;
  if (h2 == NULL || h2->streams == NULL) return false;
  for (index = 0u; index < h2->stream_capacity; ++index)
    if (chttp_server_deferred_token_state(
            atomic_load_explicit(&h2->streams[index].deferred_token, memory_order_acquire)) !=
        CHTTP_SERVER_DEFERRED_IDLE)
      return true;
  return false;
}

int chttp_h2_server_connection_deferred_progress(chttp_h2_server_connection *h2) {
  size_t index;
  if (h2 == NULL) return SALTS_OK;
  if (h2->streams == NULL || h2->connection == NULL) return SALTS_EINVAL;
  for (index = 0u; index < h2->stream_capacity; ++index) {
    chttp_h2_server_stream *stream = &h2->streams[index];
    uint_fast64_t token = atomic_load_explicit(&stream->deferred_token, memory_order_acquire);
    const chttp_server_deferred_state state = chttp_server_deferred_token_state(token);
    int status;
    if (state == CHTTP_SERVER_DEFERRED_IDLE || state == CHTTP_SERVER_DEFERRED_WRITING) continue;
    if (stream->protocol_closed || !h2->connection->active || !h2->connection->connected) {
      if (!atomic_compare_exchange_strong_explicit(
              &stream->deferred_token, &token,
              chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                                          CHTTP_SERVER_DEFERRED_IDLE),
              memory_order_acq_rel, memory_order_acquire))
        continue;
      chttp_session_request_abort(&stream->request_state);
      chttp_h2_server_stream_reset(stream);
      stream->owner = h2;
      continue;
    }
    if (h2->connection->writing || h2->connection->outbound_size != 0u) continue;
    if (state == CHTTP_SERVER_DEFERRED_CANCELED) {
      const int32_t stream_id = stream->stream_id;
      atomic_store_explicit(
          &stream->deferred_token,
          chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                                      CHTTP_SERVER_DEFERRED_IDLE),
          memory_order_release);
      chttp_session_request_abort(&stream->request_state);
      if (chttp_h2_proto_submit_rst_stream(h2->protocol, stream_id, CHTTP_H2_ERR_CANCEL) != 0) {
        chttp_server_connection_close(h2->connection);
        return SALTS_OK;
      }
      status = chttp_server_send_pending(h2->connection);
      return status == SALTS_ENOBUFS || status == SALTS_EBUSY ? SALTS_OK : status;
    }
    if (state != CHTTP_SERVER_DEFERRED_READY) continue;
    status = chttp_session_request_finish(&stream->request_state);
    atomic_store_explicit(&stream->deferred_token,
                          chttp_server_deferred_token(chttp_server_deferred_token_generation(token),
                                                      CHTTP_SERVER_DEFERRED_IDLE),
                          memory_order_release);
    if (status == SALTS_OK)
      status = chttp_h2_server_submit_response_builder(stream, &stream->deferred_builder);
    if (status != SALTS_OK) {
      chttp_h2_proto *protocol = h2->protocol;
      const int32_t stream_id = stream->stream_id;
      chttp_server_stats_handler_error(h2->connection->server);
      chttp_session_request_abort(&stream->request_state);
      if (chttp_h2_proto_submit_rst_stream(protocol, stream_id, CHTTP_H2_ERR_INTERNAL_ERROR) != 0) {
        chttp_server_connection_close(h2->connection);
        return SALTS_OK;
      }
    }
    if (!h2->connection->active) return SALTS_OK;
    status = chttp_server_send_pending(h2->connection);
    return status == SALTS_ENOBUFS || status == SALTS_EBUSY ? SALTS_OK : status;
  }
  return SALTS_OK;
}

int chttp_h2_server_connection_flush(chttp_h2_server_connection *h2) {
  chttp_server_connection *connection;
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  if (h2 == NULL || h2->protocol == NULL) return SALTS_EINVAL;
  connection = h2->connection;
  if (connection->outbound_size != 0u || connection->writing) return SALTS_OK;
  {
    const int websocket_status = chttp_h2_server_websockets_drive(h2);
    if (websocket_status != SALTS_OK) return websocket_status;
  }
  /* A matching ACK crosses the wire after every preceding response byte and
   * avoids closing a Windows socket while late flow-control frames are still
   * in flight. */
  if (h2->draining && h2->active_streams == 0u && !h2->drain_ping_sent &&
      !chttp_h2_proto_want_write(h2->protocol)) {
    if (chttp_h2_proto_submit_ping(h2->protocol, CHTTP_H2_SERVER_DRAIN_PING) != 0)
      return SALTS_EPROTO;
    h2->drain_ping_sent = true;
  }
  wire_size = chttp_h2_proto_send(h2->protocol, &wire);
  if (wire_size < 0) return SALTS_EPROTO;
  {
    const int status = chttp_server_connection_reserve_outbound(connection, (size_t)wire_size);
    if (status != SALTS_OK) return status;
  }
  if (wire_size != 0) memcpy(connection->outbound, wire, (size_t)wire_size);
  connection->outbound_size = (size_t)wire_size;
  return SALTS_OK;
}

int chttp_h2_server_connection_begin_stop(chttp_h2_server_connection *h2) {
  if (h2 == NULL || h2->protocol == NULL || h2->draining) return SALTS_OK;
  if (chttp_h2_proto_submit_goaway(h2->protocol,
                                   chttp_h2_proto_get_last_proc_stream_id(h2->protocol),
                                   CHTTP_H2_ERR_NO_ERROR) != 0)
    return SALTS_EPROTO;
  h2->draining = true;
  h2->drain_ping_sent = false;
  h2->drain_ping_acked = false;
  return SALTS_OK;
}

bool chttp_h2_server_connection_draining(const chttp_h2_server_connection *h2) {
  return h2 != NULL && h2->draining;
}

bool chttp_h2_server_connection_stop_ready(const chttp_h2_server_connection *h2) {
  return h2 != NULL && h2->protocol != NULL && h2->draining && h2->drain_ping_acked &&
         h2->active_streams == 0u && !chttp_h2_server_connection_has_deferred(h2) &&
         !chttp_h2_proto_want_write(h2->protocol) && h2->connection->outbound_size == 0u &&
         !h2->connection->writing;
}

bool chttp_h2_server_connection_stop_waiting(const chttp_h2_server_connection *h2) {
  return h2 != NULL && h2->protocol != NULL && h2->draining && h2->drain_ping_sent &&
         !h2->drain_ping_acked && h2->active_streams == 0u &&
         !chttp_h2_server_connection_has_deferred(h2) && !chttp_h2_proto_want_write(h2->protocol) &&
         h2->connection->outbound_size == 0u && !h2->connection->writing;
}
