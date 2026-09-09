#include "chttp_h2_session.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  CHTTP_H2_MIN_FRAME_BYTES = 16 * 1024,
  CHTTP_H2_FRAME_HEADER_BYTES = 9,
  CHTTP_H2_CLIENT_PREFACE_BYTES = 24,
  CHTTP_H2_SETTINGS_PAYLOAD_BYTES = 42,
  CHTTP_H2_DEFAULT_INPUT_BYTES = 128 * 1024,
  CHTTP_H2_DEFAULT_HPACK_TABLE_BYTES = 4096,
  CHTTP_H2_DEFAULT_MAX_SETTINGS_COUNT = 32,
  CHTTP_H2_HEADER_FIELD_OVERHEAD = 32
};

static bool chttp_h2_size_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || left > SIZE_MAX - right) return false;
  *out = left + right;
  return true;
}

static char *chttp_h2_copy_text(const char *text) {
  size_t size;
  char *copy;
  if (text == NULL) return NULL;
  size = strlen(text);
  if (size == SIZE_MAX) return NULL;
  copy = (char *)malloc(size + 1u);
  if (copy != NULL) memcpy(copy, text, size + 1u);
  return copy;
}

static unsigned char chttp_h2_ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static bool chttp_h2_ascii_equal_n(const char *left, size_t left_size, const char *right) {
  size_t index;
  const size_t right_size = strlen(right);
  if (left == NULL || left_size != right_size) return false;
  for (index = 0u; index < left_size; ++index)
    if (chttp_h2_ascii_lower((unsigned char)left[index]) != (unsigned char)right[index])
      return false;
  return true;
}

static const char *chttp_h2_method_name(chttp_method method) {
  switch (method) {
  case CHTTP_METHOD_GET:
    return "GET";
  case CHTTP_METHOD_HEAD:
    return "HEAD";
  case CHTTP_METHOD_POST:
    return "POST";
  case CHTTP_METHOD_PUT:
    return "PUT";
  case CHTTP_METHOD_DELETE:
    return "DELETE";
  case CHTTP_METHOD_PATCH:
    return "PATCH";
  case CHTTP_METHOD_OPTIONS:
    return "OPTIONS";
  default:
    return NULL;
  }
}

static int chttp_h2_bounded_length(const char *text, size_t limit, size_t *out_size) {
  size_t index;
  if (text == NULL || out_size == NULL) return SALTS_EINVAL;
  for (index = 0u; index <= limit; ++index) {
    if (text[index] == '\0') {
      *out_size = index;
      return SALTS_OK;
    }
  }
  return SALTS_EMSGSIZE;
}

static bool chttp_h2_header_name_byte(unsigned char value) {
  if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9'))
    return true;
  return value == '!' || value == '#' || value == '$' || value == '%' || value == '&' ||
         value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' ||
         value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

static bool chttp_h2_header_value_valid(const char *value, size_t value_size) {
  size_t index;
  if (value == NULL) return false;
  if (value_size != 0u && (value[0] == ' ' || value[0] == '\t' || value[value_size - 1u] == ' ' ||
                           value[value_size - 1u] == '\t'))
    return false;
  for (index = 0u; index < value_size; ++index) {
    const unsigned char byte = (unsigned char)value[index];
    if ((byte < 0x20u && byte != '\t') || byte == 0x7fu) return false;
  }
  return true;
}

static bool chttp_h2_header_list_add(size_t name_size, size_t value_size, size_t *total) {
  size_t field_bytes;
  return chttp_h2_size_add(name_size, value_size, &field_bytes) &&
         chttp_h2_size_add(field_bytes, CHTTP_H2_HEADER_FIELD_OVERHEAD, &field_bytes) &&
         chttp_h2_size_add(*total, field_bytes, total);
}

static int chttp_h2_request_inputs(const chttp_request_options *options,
                                   const chttp_limits *limits) {
  const char *method_name;
  size_t target_size = 0u;
  size_t authority_size = 0u;
  size_t header_bytes = 0u;
  size_t body_size_chars;
  size_t index;
  size_t generated_header_count;
  size_t declared_body_size;
  char body_size_text[3u * sizeof(size_t) + 1u];
  int status;
  if (options == NULL || limits == NULL || options->connection_uri == NULL ||
      options->authority == NULL || options->target == NULL || options->on_complete == NULL ||
      chttp_h2_method_name(options->method) == NULL ||
      (options->header_count != 0u && options->headers == NULL) ||
      (options->body_size != 0u && options->body == NULL) ||
      (options->body_source != NULL &&
       (options->body != NULL || options->body_size != 0u || options->body_source->read == NULL ||
        (options->body_source->content_length_known != 0 &&
         options->body_source->content_length_known != 1))) ||
      (options->body_sink != NULL && options->body_sink->write == NULL))
    return SALTS_EINVAL;
  declared_body_size = options->body_source != NULL && options->body_source->content_length_known
                           ? options->body_source->content_length
                           : options->body_size;
  if (declared_body_size > limits->max_request_body_bytes) return SALTS_EMSGSIZE;
  method_name = chttp_h2_method_name(options->method);
  status = chttp_h2_bounded_length(options->target, limits->max_start_line_bytes, &target_size);
  if (status != SALTS_OK) return status;
  if (target_size == 0u ||
      (options->target[0] != '/' && !(options->method == CHTTP_METHOD_OPTIONS &&
                                      target_size == 1u && options->target[0] == '*')))
    return SALTS_EINVAL;
  for (index = 0u; index < target_size; ++index) {
    const unsigned char value = (unsigned char)options->target[index];
    if (value <= 0x20u || value >= 0x7fu || value == '#') return SALTS_EINVAL;
  }
  status = chttp_h2_bounded_length(options->authority, limits->max_header_bytes, &authority_size);
  if (status != SALTS_OK) return status;
  if (authority_size == 0u) return SALTS_EINVAL;
  for (index = 0u; index < authority_size; ++index) {
    const unsigned char value = (unsigned char)options->authority[index];
    if (value <= 0x20u || value >= 0x7fu || value == '/' || value == '?' || value == '#' ||
        value == '@')
      return SALTS_EINVAL;
  }
  generated_header_count =
      options->body_source != NULL && !options->body_source->content_length_known ? 4u : 5u;
  if (options->header_count > SIZE_MAX - generated_header_count ||
      options->header_count + generated_header_count > limits->max_header_count)
    return SALTS_EMSGSIZE;
  status = snprintf(body_size_text, sizeof(body_size_text), "%zu", declared_body_size);
  if (status <= 0 || (size_t)status >= sizeof(body_size_text)) return SALTS_ERANGE;
  body_size_chars = (size_t)status;
  if (!chttp_h2_header_list_add(sizeof(":method") - 1u, strlen(method_name), &header_bytes) ||
      !chttp_h2_header_list_add(sizeof(":scheme") - 1u,
                                strncmp(options->connection_uri, "tls://", sizeof("tls://") - 1u) ==
                                        0
                                    ? sizeof("https") - 1u
                                    : sizeof("http") - 1u,
                                &header_bytes) ||
      !chttp_h2_header_list_add(sizeof(":path") - 1u, target_size, &header_bytes) ||
      !chttp_h2_header_list_add(sizeof(":authority") - 1u, authority_size, &header_bytes))
    return SALTS_EMSGSIZE;
  if (generated_header_count == 5u &&
      !chttp_h2_header_list_add(sizeof("content-length") - 1u, body_size_chars, &header_bytes))
    return SALTS_EMSGSIZE;
  for (index = 0u; index < options->header_count; ++index) {
    const chttp_header *header = &options->headers[index];
    size_t name_size = 0u;
    size_t value_size = 0u;
    size_t byte_index;
    status = chttp_h2_bounded_length(header->name, limits->max_header_bytes, &name_size);
    if (status != SALTS_OK) return status;
    status = chttp_h2_bounded_length(header->value, limits->max_header_bytes, &value_size);
    if (status != SALTS_OK) return status;
    if (name_size == 0u || header->name[0] == ':' ||
        chttp_h2_ascii_equal_n(header->name, name_size, "host") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "content-length") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "connection") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "proxy-connection") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "keep-alive") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "transfer-encoding") ||
        chttp_h2_ascii_equal_n(header->name, name_size, "upgrade"))
      return SALTS_EINVAL;
    for (byte_index = 0u; byte_index < name_size; ++byte_index)
      if (!chttp_h2_header_name_byte((unsigned char)header->name[byte_index])) return SALTS_EINVAL;
    if (!chttp_h2_header_value_valid(header->value, value_size) ||
        (chttp_h2_ascii_equal_n(header->name, name_size, "te") &&
         !chttp_h2_ascii_equal_n(header->value, value_size, "trailers")))
      return SALTS_EINVAL;
    if (!chttp_h2_header_list_add(name_size, value_size, &header_bytes)) return SALTS_EMSGSIZE;
  }
  return header_bytes <= limits->max_header_bytes ? SALTS_OK : SALTS_EMSGSIZE;
}

int chttp_h2_protocol_config(const chttp_client_config *config, chttp_h2_proto_config *out_config) {
  size_t input_buffer_bytes;
  size_t hpack_dynamic_table_bytes;
  size_t max_settings_count;
  size_t minimum_output;
  if (config == NULL || out_config == NULL) return SALTS_EINVAL;
  input_buffer_bytes = config->h2_input_buffer_bytes;
  if (input_buffer_bytes == 0u) {
    input_buffer_bytes = config->network.receive_buffer_bytes > CHTTP_H2_DEFAULT_INPUT_BYTES
                             ? config->network.receive_buffer_bytes
                             : CHTTP_H2_DEFAULT_INPUT_BYTES;
  }
  hpack_dynamic_table_bytes = config->h2_hpack_dynamic_table_bytes != 0u
                                  ? config->h2_hpack_dynamic_table_bytes
                                  : CHTTP_H2_DEFAULT_HPACK_TABLE_BYTES;
  max_settings_count = config->h2_max_settings_count != 0u ? config->h2_max_settings_count
                                                           : CHTTP_H2_DEFAULT_MAX_SETTINGS_COUNT;
  minimum_output = CHTTP_H2_CLIENT_PREFACE_BYTES + 2u * CHTTP_H2_FRAME_HEADER_BYTES +
                   CHTTP_H2_SETTINGS_PAYLOAD_BYTES + CHTTP_H2_MIN_FRAME_BYTES;
  if (config->network.max_send_bytes < minimum_output || config->max_header_bytes == 0u ||
      config->max_header_bytes > config->network.max_send_bytes - CHTTP_H2_FRAME_HEADER_BYTES ||
      input_buffer_bytes < CHTTP_H2_FRAME_HEADER_BYTES + CHTTP_H2_MIN_FRAME_BYTES ||
      input_buffer_bytes > PTRDIFF_MAX || hpack_dynamic_table_bytes > UINT32_MAX ||
      max_settings_count > SIZE_MAX / sizeof(uint32_t))
    return SALTS_EMSGSIZE;
  *out_config = (chttp_h2_proto_config){.stream_capacity = config->request_capacity,
                                        .output_buffer_bytes = config->network.max_send_bytes,
                                        .input_buffer_bytes = input_buffer_bytes,
                                        .header_block_bytes = config->max_header_bytes,
                                        .max_header_list_bytes = config->max_header_bytes,
                                        .hpack_dynamic_table_bytes = hpack_dynamic_table_bytes,
                                        .max_hpack_string_bytes = config->max_header_bytes,
                                        .max_settings_count = max_settings_count};
  return SALTS_OK;
}

int chttp_h2_request_prepare(chttp_h2_request_state *request, const chttp_request_options *options,
                             const chttp_limits *limits, void *request_user) {
  size_t storage_capacity;
  int status;
  if (request == NULL || options == NULL || limits == NULL) return SALTS_EINVAL;
  status = chttp_h2_request_inputs(options, limits);
  if (status != SALTS_OK) return status;
  if (limits->max_header_count > (SIZE_MAX - limits->max_header_bytes - 1u) / 2u)
    return SALTS_ERANGE;
  storage_capacity = limits->max_header_bytes + limits->max_header_count * 2u + 1u;
  memset(request, 0, sizeof(*request));
  request->headers = (chttp_header *)calloc(limits->max_header_count, sizeof(*request->headers));
  request->header_storage = (char *)malloc(storage_capacity);
  request->response_body =
      options->body_sink == NULL ? (unsigned char *)malloc(limits->max_response_body_bytes) : NULL;
  if (options->body_size != 0u) request->request_body = (unsigned char *)malloc(options->body_size);
  if (request->headers == NULL || request->header_storage == NULL ||
      (options->body_sink == NULL && request->response_body == NULL) ||
      (options->body_size != 0u && request->request_body == NULL)) {
    chttp_h2_request_destroy(request);
    return SALTS_ENOMEM;
  }
  if (options->body_size != 0u) memcpy(request->request_body, options->body, options->body_size);
  request->request_user = request_user;
  request->method = options->method;
  request->header_storage_capacity = storage_capacity;
  request->max_header_count = limits->max_header_count;
  request->max_header_bytes = limits->max_header_bytes;
  request->max_response_body_bytes = limits->max_response_body_bytes;
  request->max_informational_responses = limits->max_informational_responses;
  request->max_request_body_bytes = limits->max_request_body_bytes;
  request->stream_chunk_bytes = limits->stream_chunk_bytes;
  if (options->body_source != NULL) {
    request->body_source = *options->body_source;
    request->source_enabled = true;
  }
  if (options->body_sink != NULL) {
    request->body_sink = *options->body_sink;
    request->sink_enabled = true;
  }
  request->response.http_major = 2u;
  request->response.http_minor = 0u;
  request->response.protocol_keep_alive = 1;
  return SALTS_OK;
}

void chttp_h2_request_destroy(chttp_h2_request_state *request) {
  if (request == NULL) return;
  if (request->file_transfer != NULL)
    chttp_file_transfer_set_ready(request->file_transfer, NULL, NULL);
  if (request->file_sink_transfer != NULL)
    chttp_file_sink_transfer_set_ready(request->file_sink_transfer, NULL, NULL);
  free(request->request_body);
  free(request->response_body);
  free(request->header_storage);
  free(request->headers);
  memset(request, 0, sizeof(*request));
}

static chttp_h2_request_state *chttp_h2_session_request(chttp_h2_session *session,
                                                        int32_t stream_id) {
  size_t index;
  if (session == NULL) return NULL;
  for (index = 0u; index < session->request_capacity; ++index) {
    chttp_h2_request_state *request = session->requests[index];
    if (request != NULL && request->stream_id == stream_id) return request;
  }
  return NULL;
}

static void chttp_h2_session_complete(chttp_h2_session *session, chttp_h2_request_state *request,
                                      const chttp_response_view *response, int status,
                                      int native_status, const char *stage) {
  if (session == NULL || request == NULL || request->completed) return;
  request->completed = true;
  if (request->registry_index < session->request_capacity &&
      session->requests[request->registry_index] == request)
    session->requests[request->registry_index] = NULL;
  if (session->active_requests != 0u) --session->active_requests;
  session->callbacks.on_complete(session->callbacks.user, request->request_user, response, status,
                                 native_status, stage);
  if (session->state == CHTTP_H2_SESSION_DRAINING && session->active_requests == 0u)
    session->close_after_flush = true;
}

static void chttp_h2_session_record_terminal(chttp_h2_request_state *request, int status,
                                             int native_status, const char *stage) {
  if (request == NULL || request->completed || request->terminal_pending) return;
  request->terminal_pending = true;
  request->terminal_status = status;
  request->terminal_native_status = native_status;
  request->terminal_stage = stage;
}

static void chttp_h2_session_fail_requests(chttp_h2_session *session, int status, int native_status,
                                           const char *stage) {
  size_t index;
  if (session == NULL) return;
  for (index = 0u; index < session->request_capacity; ++index) {
    chttp_h2_request_state *request = session->requests[index];
    if (request != NULL)
      chttp_h2_session_complete(session, request, NULL, status, native_status, stage);
  }
}

static int chttp_h2_response_status(const char *value, size_t value_size,
                                    unsigned int *out_status) {
  unsigned int status;
  if (value == NULL || out_status == NULL || value_size != 3u || value[0] < '1' || value[0] > '9' ||
      value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9')
    return SALTS_EPROTO;
  status = (unsigned int)(value[0] - '0') * 100u + (unsigned int)(value[1] - '0') * 10u +
           (unsigned int)(value[2] - '0');
  *out_status = status;
  return SALTS_OK;
}

static int chttp_h2_decimal_size(const char *value, size_t value_size, size_t *out_size) {
  size_t result = 0u;
  size_t index;
  if (value == NULL || value_size == 0u || out_size == NULL) return SALTS_EPROTO;
  for (index = 0u; index < value_size; ++index) {
    const unsigned char digit = (unsigned char)value[index];
    if (digit < '0' || digit > '9' || result > (SIZE_MAX - (size_t)(digit - '0')) / 10u)
      return SALTS_EPROTO;
    result = result * 10u + (size_t)(digit - '0');
  }
  *out_size = result;
  return SALTS_OK;
}

static int chttp_h2_on_begin_headers(void *user, int32_t stream_id) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  chttp_h2_request_state *request = chttp_h2_session_request(session, stream_id);
  if (request == NULL || request->header_block_open) return -1;
  request->header_block_open = true;
  request->regular_header_seen = false;
  request->status_seen = false;
  request->current_status = 0u;
  request->trailers = request->final_headers_seen;
  return 0;
}

static int chttp_h2_on_header(void *user, int32_t stream_id, const char *name, size_t name_size,
                              const char *value, size_t value_size) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  chttp_h2_request_state *request = chttp_h2_session_request(session, stream_id);
  size_t field_bytes;
  size_t required;
  size_t index;
  char *name_copy;
  char *value_copy;
  if (request == NULL || !request->header_block_open || name == NULL || value == NULL ||
      name_size == 0u)
    return -1;
  if (name[0] == ':') {
    if (request->trailers || request->regular_header_seen || request->status_seen ||
        !chttp_h2_ascii_equal_n(name, name_size, ":status") ||
        chttp_h2_response_status(value, value_size, &request->current_status) != SALTS_OK) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-response-headers";
      return -1;
    }
    request->status_seen = true;
    return 0;
  }
  for (index = 0u; index < name_size; ++index) {
    if (!chttp_h2_header_name_byte((unsigned char)name[index]) ||
        (name[index] >= 'A' && name[index] <= 'Z')) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-response-header-name";
      return -1;
    }
  }
  if (!chttp_h2_header_value_valid(value, value_size)) {
    request->failure_status = SALTS_EPROTO;
    request->failure_stage = "h2-response-header-value";
    return -1;
  }
  if (chttp_h2_ascii_equal_n(name, name_size, "connection") ||
      chttp_h2_ascii_equal_n(name, name_size, "proxy-connection") ||
      chttp_h2_ascii_equal_n(name, name_size, "keep-alive") ||
      chttp_h2_ascii_equal_n(name, name_size, "te") ||
      chttp_h2_ascii_equal_n(name, name_size, "transfer-encoding") ||
      chttp_h2_ascii_equal_n(name, name_size, "upgrade")) {
    request->failure_status = SALTS_EPROTO;
    request->failure_stage = "h2-response-connection-header";
    return -1;
  }
  if (chttp_h2_ascii_equal_n(name, name_size, "content-length")) {
    size_t content_length = 0u;
    if (request->trailers ||
        chttp_h2_decimal_size(value, value_size, &content_length) != SALTS_OK ||
        (request->content_length_seen && request->content_length != content_length)) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-content-length";
      return -1;
    }
    request->content_length_seen = true;
    request->content_length = content_length;
  }
  request->regular_header_seen = true;
  if (request->response.header_count >= request->max_header_count ||
      !chttp_h2_size_add(name_size, value_size, &field_bytes) ||
      !chttp_h2_size_add(field_bytes, CHTTP_H2_HEADER_FIELD_OVERHEAD, &field_bytes) ||
      !chttp_h2_size_add(request->header_list_bytes, field_bytes, &field_bytes) ||
      field_bytes > request->max_header_bytes ||
      !chttp_h2_size_add(name_size, value_size, &required) ||
      !chttp_h2_size_add(required, 2u, &required) ||
      required > request->header_storage_capacity - request->header_storage_used) {
    request->failure_status = SALTS_EMSGSIZE;
    request->failure_stage = "h2-response-headers";
    return -1;
  }
  name_copy = request->header_storage + request->header_storage_used;
  memcpy(name_copy, name, name_size);
  name_copy[name_size] = '\0';
  value_copy = name_copy + name_size + 1u;
  memcpy(value_copy, value, value_size);
  value_copy[value_size] = '\0';
  request->header_storage_used += required;
  request->header_list_bytes = field_bytes;
  request->headers[request->response.header_count++] = (chttp_header){name_copy, value_copy};
  request->response.headers = request->headers;
  return 0;
}

static int chttp_h2_on_end_headers(void *user, int32_t stream_id, int end_stream) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  chttp_h2_request_state *request = chttp_h2_session_request(session, stream_id);
  if (request == NULL || !request->header_block_open) return -1;
  request->header_block_open = false;
  if (request->trailers) {
    if (request->status_seen || !end_stream) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-response-trailers";
      return -1;
    }
    return 0;
  }
  if (!request->status_seen) {
    request->failure_status = SALTS_EPROTO;
    request->failure_stage = "h2-response-status";
    return -1;
  }
  if (request->current_status < 200u) {
    if (end_stream || request->current_status == 101u) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-informational-response";
      return -1;
    }
    if (request->informational_responses >= request->max_informational_responses) {
      request->failure_status = SALTS_EMSGSIZE;
      request->failure_stage = "h2-informational-response";
      return -1;
    }
    ++request->informational_responses;
    request->response.header_count = 0u;
    request->response.headers = request->headers;
    request->header_storage_used = 0u;
    request->header_list_bytes = 0u;
    request->content_length_seen = false;
    request->content_length = 0u;
    return 0;
  }
  request->response.status_code = request->current_status;
  request->final_headers_seen = true;
  return 0;
}

static int chttp_h2_on_data(void *user, int32_t stream_id, const uint8_t *data, size_t size) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  chttp_h2_request_state *request = chttp_h2_session_request(session, stream_id);
  size_t next_size = 0u;
  if (request == NULL || !request->final_headers_seen || (size != 0u && data == NULL) ||
      ((request->method == CHTTP_METHOD_HEAD || request->response.status_code == 204u ||
        request->response.status_code == 304u) &&
       size != 0u)) {
    if (request != NULL) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "h2-response-body";
    }
    return -1;
  }
  if (!chttp_h2_size_add(request->response.body_size, size, &next_size) ||
      next_size > request->max_response_body_bytes) {
    const bool was_deferred = session->defer_completions;
    request->failure_status = SALTS_EMSGSIZE;
    request->failure_stage = "h2-response-body";
    if (size != 0u && chttp_h2_proto_consume_connection(session->protocol, size) != 0) return -1;
    session->defer_completions = true;
    if (chttp_h2_proto_submit_rst_stream(session->protocol, stream_id,
                                         CHTTP_H2_ERR_ENHANCE_YOUR_CALM) != 0) {
      session->defer_completions = was_deferred;
      return -1;
    }
    session->defer_completions = was_deferred;
    return 0;
  }
  if (size != 0u && request->file_sink_transfer != NULL) {
    const chttp_file_sink_result sink_result =
        chttp_file_sink_transfer_write(request->file_sink_transfer, data, size);
    if (sink_result == CHTTP_FILE_SINK_ERROR) {
      const bool was_deferred = session->defer_completions;
      int native_status = 0;
      request->failure_status =
          chttp_file_sink_transfer_status(request->file_sink_transfer, &native_status);
      if (request->failure_status == SALTS_OK) request->failure_status = SALTS_EIO;
      request->terminal_native_status = native_status;
      request->failure_stage = "file-write";
      if (chttp_h2_proto_consume_connection(session->protocol, size) != 0) return -1;
      session->defer_completions = true;
      if (chttp_h2_proto_submit_rst_stream(session->protocol, stream_id, CHTTP_H2_ERR_CANCEL) !=
          0) {
        session->defer_completions = was_deferred;
        return -1;
      }
      session->defer_completions = was_deferred;
      return CHTTP_H2_PROTO_DATA_OK;
    }
    request->response.body_size = next_size;
    request->response.body = NULL;
    request->sink_flow_bytes = size;
    request->sink_write_pending = true;
    return CHTTP_H2_PROTO_DATA_PAUSE;
  }
  if (size != 0u && request->sink_enabled) {
    const int sink_status = request->body_sink.write(request->body_sink.user, data, size);
    if (sink_status != SALTS_OK) {
      const bool was_deferred = session->defer_completions;
      request->failure_status = sink_status;
      request->failure_stage = "response-sink";
      if (chttp_h2_proto_consume_connection(session->protocol, size) != 0) return -1;
      session->defer_completions = true;
      if (chttp_h2_proto_submit_rst_stream(session->protocol, stream_id, CHTTP_H2_ERR_CANCEL) !=
          0) {
        session->defer_completions = was_deferred;
        return -1;
      }
      session->defer_completions = was_deferred;
      return 0;
    }
  } else if (size != 0u) {
    memcpy(request->response_body + request->response.body_size, data, size);
  }
  request->response.body_size = next_size;
  request->response.body =
      !request->sink_enabled && next_size != 0u ? request->response_body : NULL;
  if (size != 0u && (chttp_h2_proto_consume_stream(session->protocol, stream_id, size) != 0 ||
                     chttp_h2_proto_consume_connection(session->protocol, size) != 0)) {
    request->failure_status = SALTS_EPROTO;
    request->failure_stage = "h2-flow-control";
    return -1;
  }
  return 0;
}

static int chttp_h2_on_stream_close(void *user, int32_t stream_id, uint32_t error_code) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  chttp_h2_request_state *request = chttp_h2_session_request(session, stream_id);
  int status;
  const char *stage;
  const bool content_length_matches =
      request != NULL &&
      (!request->content_length_seen || request->method == CHTTP_METHOD_HEAD ||
       request->response.status_code == 204u || request->response.status_code == 304u ||
       request->content_length == request->response.body_size);
  if (request == NULL || request->completed) return 0;
  if (error_code == CHTTP_H2_ERR_NO_ERROR && request->final_headers_seen &&
      content_length_matches) {
    status = SALTS_OK;
    stage = NULL;
  } else if (error_code == CHTTP_H2_ERR_NO_ERROR && request->final_headers_seen) {
    status = SALTS_EPROTO;
    stage = "h2-content-length";
  } else if (request->failure_status != SALTS_OK) {
    status = request->failure_status;
    stage = request->failure_stage;
  } else if (error_code == CHTTP_H2_ERR_CANCEL) {
    status = SALTS_ECANCELED;
    stage = "h2-cancel";
  } else {
    status = error_code == CHTTP_H2_ERR_REFUSED_STREAM ? SALTS_ECONNRESET : SALTS_EPROTO;
    stage = error_code == CHTTP_H2_ERR_REFUSED_STREAM ? "h2-refused-stream" : "h2-stream";
  }
  {
    const int native_status =
        request->failure_status != SALTS_OK ? request->terminal_native_status : (int)error_code;
    if (session->defer_completions) {
      chttp_h2_session_record_terminal(request, status, native_status, stage);
    } else {
      chttp_h2_session_complete(session, request, status == SALTS_OK ? &request->response : NULL,
                                status, native_status, stage);
    }
  }
  return 0;
}

static void chttp_h2_on_goaway(void *user, uint32_t last_stream_id, uint32_t error_code) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  (void)last_stream_id;
  (void)error_code;
  if (session == NULL) return;
  if (session->state == CHTTP_H2_SESSION_ACTIVE) session->state = CHTTP_H2_SESSION_DRAINING;
  if (session->active_requests == 0u) session->close_after_flush = true;
}

static int chttp_h2_session_try_close(chttp_h2_session *session) {
  int status;
  if (session == NULL || session->state == CHTTP_H2_SESSION_FREE ||
      session->state == CHTTP_H2_SESSION_TERMINAL || session->close_admitted)
    return SALTS_OK;
  status = cnet_close(session->network, session->connection);
  if (status == SALTS_OK || status == SALTS_EALREADY || status == SALTS_ENOENT ||
      status == SALTS_ESHUTDOWN) {
    session->close_admitted = true;
    session->close_pending = false;
    session->state = CHTTP_H2_SESSION_CLOSING;
    return SALTS_OK;
  }
  if (status == SALTS_ENOBUFS || status == SALTS_EBUSY) {
    session->close_pending = true;
    return SALTS_OK;
  }
  return status;
}

static void chttp_h2_session_fail(chttp_h2_session *session, int status, int native_status,
                                  const char *stage) {
  if (session == NULL || session->state == CHTTP_H2_SESSION_TERMINAL) return;
  chttp_h2_session_fail_requests(session, status, native_status, stage);
  session->state = CHTTP_H2_SESSION_DRAINING;
  session->close_after_flush = false;
  (void)chttp_h2_session_try_close(session);
}

static int chttp_h2_session_flush(chttp_h2_session *session) {
  const uint8_t *wire = NULL;
  ptrdiff_t wire_size;
  int status;
  if (session == NULL || session->state == CHTTP_H2_SESSION_FREE ||
      session->state == CHTTP_H2_SESSION_CONNECTING ||
      session->state == CHTTP_H2_SESSION_TERMINAL || session->send_active)
    return SALTS_OK;
  if (session->pending_output_size == 0u && chttp_h2_proto_want_write(session->protocol)) {
    wire_size = chttp_h2_proto_send(session->protocol, &wire);
    if (wire_size < 0 || (size_t)wire_size > session->protocol_config.output_buffer_bytes) {
      chttp_h2_session_fail(session, SALTS_EPROTO, 0, "h2-write");
      return SALTS_EPROTO;
    }
    if (wire_size > 0) {
      memcpy(session->pending_output, wire, (size_t)wire_size);
      session->pending_output_size = (size_t)wire_size;
    }
  }
  if (session->pending_output_size != 0u) {
    status = cnet_send(session->network, session->connection, session->pending_output,
                       session->pending_output_size);
    if (status == SALTS_EBUSY || status == SALTS_ENOBUFS) return SALTS_OK;
    if (status != SALTS_OK) {
      chttp_h2_session_fail(session, status, 0, "h2-send-admission");
      return status;
    }
    session->pending_output_size = 0u;
    session->send_active = true;
  }
  if (session->close_after_flush && session->pending_output_size == 0u && !session->send_active &&
      !chttp_h2_proto_want_write(session->protocol))
    return chttp_h2_session_try_close(session);
  return SALTS_OK;
}

static int chttp_h2_session_arm_receive(chttp_h2_session *session) {
  int status;
  if (session->receive_armed || session->close_admitted) return SALTS_OK;
  status = cnet_receive(session->network, session->connection, 1u);
  if (status == SALTS_OK) session->receive_armed = true;
  return status;
}

static void chttp_h2_cnet_state(void *user, cnet_connection connection, cnet_connection_state state,
                                const cnet_error *error) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  int status;
  if (session == NULL || session->connection.slot != connection.slot ||
      session->connection.generation != connection.generation ||
      session->state == CHTTP_H2_SESSION_FREE || session->state == CHTTP_H2_SESSION_TERMINAL)
    return;
  if (state == CNET_CONNECTION_CONNECTED) {
    if (session->state != CHTTP_H2_SESSION_CONNECTING) return;
    if (session->tls) {
      char alpn[sizeof("h2")];
      size_t alpn_size = 0u;
      status =
          cnet_tls_negotiated_alpn(session->network, connection, alpn, sizeof(alpn), &alpn_size);
      if (status != SALTS_OK || alpn_size != sizeof("h2") - 1u ||
          memcmp(alpn, "h2", sizeof("h2")) != 0) {
        chttp_h2_session_fail(session, SALTS_EPROTONOSUPPORT, status, "h2-alpn");
        return;
      }
    }
    session->state = CHTTP_H2_SESSION_ACTIVE;
    status = chttp_h2_session_arm_receive(session);
    if (status != SALTS_OK) {
      chttp_h2_session_fail(session, status, 0, "h2-receive-admission");
      return;
    }
    (void)chttp_h2_session_flush(session);
    return;
  }
  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;
  session->receive_armed = false;
  session->send_active = false;
  if (session->active_requests != 0u) {
    const int failure_status =
        state == CNET_CONNECTION_FAILED && error != NULL ? error->status : SALTS_ECONNRESET;
    const int native_status = error != NULL ? error->native_status : 0;
    const char *stage = error != NULL && error->stage != NULL ? error->stage : "h2-transport";
    chttp_h2_session_fail_requests(session, failure_status, native_status, stage);
  }
  session->state = CHTTP_H2_SESSION_TERMINAL;
  session->close_pending = false;
}

static void chttp_h2_cnet_receive(void *user, cnet_connection connection,
                                  const cnet_receive_view *view) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  ptrdiff_t consumed;
  int status;
  if (session == NULL || view == NULL || session->connection.slot != connection.slot ||
      session->connection.generation != connection.generation ||
      (session->state != CHTTP_H2_SESSION_ACTIVE && session->state != CHTTP_H2_SESSION_DRAINING))
    return;
  session->receive_armed = false;
  if (view->kind != CNET_MESSAGE_BYTES) {
    chttp_h2_session_fail(session, SALTS_ENOTSUP, 0, "h2-transport-kind");
    return;
  }
  consumed = chttp_h2_proto_recv(session->protocol, (const uint8_t *)view->data, view->size);
  if (consumed < 0 || (size_t)consumed != view->size) {
    chttp_h2_session_fail(session, SALTS_EPROTO, 0, "h2-protocol");
    return;
  }
  if (!chttp_h2_proto_input_paused(session->protocol)) {
    status = chttp_h2_session_arm_receive(session);
    if (status != SALTS_OK) {
      chttp_h2_session_fail(session, status, 0, "h2-receive-admission");
      return;
    }
  }
  (void)chttp_h2_session_flush(session);
}

static void chttp_h2_cnet_send(void *user, cnet_connection connection, size_t size) {
  chttp_h2_session *session = (chttp_h2_session *)user;
  (void)size;
  if (session == NULL || session->connection.slot != connection.slot ||
      session->connection.generation != connection.generation ||
      session->state == CHTTP_H2_SESSION_FREE || session->state == CHTTP_H2_SESSION_TERMINAL)
    return;
  session->send_active = false;
  (void)chttp_h2_session_flush(session);
}

int chttp_h2_session_open(chttp_h2_session *session, cnet_client *network,
                          const chttp_request_options *options, chttp_tls_profile_impl *tls_profile,
                          const chttp_h2_proto_config *protocol_config, const chttp_limits *limits,
                          const chttp_h2_session_callbacks *callbacks) {
  chttp_h2_proto_callbacks protocol_callbacks = {0};
  cnet_connect_options connect_options;
  int status;
  if (session == NULL || network == NULL || options == NULL || protocol_config == NULL ||
      limits == NULL || callbacks == NULL || callbacks->on_complete == NULL ||
      session->state != CHTTP_H2_SESSION_FREE)
    return SALTS_EINVAL;
  memset(session, 0, sizeof(*session));
  session->network = network;
  session->limits = *limits;
  session->protocol_config = *protocol_config;
  session->callbacks = *callbacks;
  session->request_capacity = protocol_config->stream_capacity;
  session->tls = strncmp(options->connection_uri, "tls://", sizeof("tls://") - 1u) == 0;
  session->connection_uri = chttp_h2_copy_text(options->connection_uri);
  session->authority = chttp_h2_copy_text(options->authority);
  session->requests =
      (chttp_h2_request_state **)calloc(session->request_capacity, sizeof(*session->requests));
  session->pending_output = (unsigned char *)malloc(protocol_config->output_buffer_bytes);
  if (session->connection_uri == NULL || session->authority == NULL || session->requests == NULL ||
      session->pending_output == NULL) {
    chttp_h2_session_destroy(session);
    chttp_tls_profile_release(tls_profile);
    return SALTS_ENOMEM;
  }
  session->tls_profile = tls_profile;
  protocol_callbacks.user_data = session;
  protocol_callbacks.on_begin_headers = chttp_h2_on_begin_headers;
  protocol_callbacks.on_header = chttp_h2_on_header;
  protocol_callbacks.on_end_headers = chttp_h2_on_end_headers;
  protocol_callbacks.on_data = chttp_h2_on_data;
  protocol_callbacks.on_stream_close = chttp_h2_on_stream_close;
  protocol_callbacks.on_goaway = chttp_h2_on_goaway;
  session->protocol =
      chttp_h2_proto_create(CHTTP_H2_PROTO_CLIENT, protocol_config, &protocol_callbacks);
  if (session->protocol == NULL) {
    chttp_h2_session_destroy(session);
    return SALTS_ENOMEM;
  }
  chttp_h2_proto_set_send_chunk(session->protocol, limits->stream_chunk_bytes);
  connect_options =
      (cnet_connect_options){.uri = options->connection_uri,
                             .observer = {.on_state = chttp_h2_cnet_state,
                                          .on_receive = chttp_h2_cnet_receive,
                                          .user = session,
                                          .on_send = chttp_h2_cnet_send},
                             .tls_client = chttp_tls_profile_client(session->tls_profile)};
  status = cnet_connect(network, &connect_options, &session->connection);
  if (status != SALTS_OK) {
    chttp_h2_session_destroy(session);
    return status;
  }
  session->state = CHTTP_H2_SESSION_CONNECTING;
  return SALTS_OK;
}

bool chttp_h2_session_matches(const chttp_h2_session *session, const chttp_request_options *options,
                              const chttp_tls_profile_impl *tls_profile) {
  return session != NULL && options != NULL &&
         (session->state == CHTTP_H2_SESSION_CONNECTING ||
          session->state == CHTTP_H2_SESSION_ACTIVE) &&
         session->connection_uri != NULL && session->authority != NULL &&
         strcmp(session->connection_uri, options->connection_uri) == 0 &&
         strcmp(session->authority, options->authority) == 0 && session->tls_profile == tls_profile;
}

bool chttp_h2_session_terminal(const chttp_h2_session *session) {
  return session != NULL && session->state == CHTTP_H2_SESSION_TERMINAL;
}

static int chttp_h2_headers_build(const chttp_request_options *options,
                                  chttp_h2_hpack_header **out_headers, char **out_names,
                                  size_t *out_count) {
  static const char status_names[] = ":method\0:scheme\0:path\0:authority\0content-length";
  const char *method = chttp_h2_method_name(options->method);
  const char *scheme =
      strncmp(options->connection_uri, "tls://", sizeof("tls://") - 1u) == 0 ? "https" : "http";
  chttp_h2_hpack_header *headers;
  char *names = NULL;
  char body_size_text[3u * sizeof(size_t) + 1u];
  size_t names_size = 0u;
  size_t storage_size;
  const bool include_content_length =
      options->body_source == NULL || options->body_source->content_length_known;
  const size_t generated_count = include_content_length ? 5u : 4u;
  size_t count = options->header_count + generated_count;
  size_t index;
  int body_size_chars;
  if (out_headers == NULL || out_names == NULL || out_count == NULL) return SALTS_EINVAL;
  *out_headers = NULL;
  *out_names = NULL;
  *out_count = 0u;
  body_size_chars = snprintf(body_size_text, sizeof(body_size_text), "%zu",
                             options->body_source != NULL ? options->body_source->content_length
                                                          : options->body_size);
  if (body_size_chars <= 0 || (size_t)body_size_chars >= sizeof(body_size_text))
    return SALTS_ERANGE;
  headers = (chttp_h2_hpack_header *)calloc(count, sizeof(*headers));
  if (headers == NULL) return SALTS_ENOMEM;
  for (index = 0u; index < options->header_count; ++index) {
    const size_t name_size = strlen(options->headers[index].name);
    if (!chttp_h2_size_add(names_size, name_size, &names_size)) {
      free(headers);
      return SALTS_EMSGSIZE;
    }
  }
  if (!chttp_h2_size_add(names_size, include_content_length ? (size_t)body_size_chars : 0u,
                         &storage_size)) {
    free(headers);
    return SALTS_EMSGSIZE;
  }
  names = (char *)malloc(storage_size != 0u ? storage_size : 1u);
  if (names == NULL) {
    free(headers);
    return SALTS_ENOMEM;
  }
  {
    size_t offset = 0u;
    for (index = 0u; index < options->header_count; ++index) {
      size_t byte_index;
      const size_t name_size = strlen(options->headers[index].name);
      for (byte_index = 0u; byte_index < name_size; ++byte_index)
        names[offset + byte_index] =
            (char)chttp_h2_ascii_lower((unsigned char)options->headers[index].name[byte_index]);
      headers[generated_count + index] =
          (chttp_h2_hpack_header){names + offset, name_size, options->headers[index].value,
                                  strlen(options->headers[index].value)};
      offset += name_size;
    }
    if (include_content_length) memcpy(names + offset, body_size_text, (size_t)body_size_chars);
  }
  headers[0] = (chttp_h2_hpack_header){status_names, 7u, method, strlen(method)};
  headers[1] = (chttp_h2_hpack_header){status_names + 8u, 7u, scheme, strlen(scheme)};
  headers[2] =
      (chttp_h2_hpack_header){status_names + 16u, 5u, options->target, strlen(options->target)};
  headers[3] = (chttp_h2_hpack_header){status_names + 22u, 10u, options->authority,
                                       strlen(options->authority)};
  if (include_content_length)
    headers[4] = (chttp_h2_hpack_header){status_names + 33u, 14u, names + names_size,
                                         (size_t)body_size_chars};
  *out_headers = headers;
  *out_names = names;
  *out_count = count;
  return SALTS_OK;
}

static chttp_h2_proto_source_result chttp_h2_request_source(void *user, uint8_t *buffer,
                                                            size_t capacity) {
  chttp_h2_request_state *request = (chttp_h2_request_state *)user;
  size_t produced = 0u;
  size_t available = 0u;
  int status;
  if (request == NULL || buffer == NULL || capacity == 0u || !request->source_enabled) {
    if (request != NULL) {
      request->failure_status = SALTS_EINVAL;
      request->failure_stage = "request-source";
    }
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  }
  if (request->stream_chunk_bytes != 0u && capacity > request->stream_chunk_bytes)
    capacity = request->stream_chunk_bytes;
  if (request->body_source.content_length_known) {
    available = request->body_source.content_length - request->source_transferred;
    if (available < capacity) capacity = available;
    if (available == 0u) capacity = request->stream_chunk_bytes;
  }
  if (request->file_transfer != NULL) {
    const chttp_file_source_result source_result =
        chttp_file_transfer_read(request->file_transfer, buffer, capacity, &produced);
    if (source_result == CHTTP_FILE_SOURCE_WAIT)
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_WAIT, 0u};
    if (source_result == CHTTP_FILE_SOURCE_ERROR) {
      int native_status = 0;
      request->failure_status = chttp_file_transfer_status(request->file_transfer, &native_status);
      if (request->failure_status == SALTS_OK) request->failure_status = SALTS_EIO;
      request->terminal_native_status = native_status;
      request->failure_stage = "file-read";
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
    if (source_result == CHTTP_FILE_SOURCE_EOF) produced = 0u;
  } else {
    status = request->body_source.read(request->body_source.user, buffer, capacity, &produced);
    if (status != SALTS_OK) {
      request->failure_status = status;
      request->failure_stage = "request-source";
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
  }
  if (produced > capacity) {
    request->failure_status = SALTS_EPROTO;
    request->failure_stage = "request-source";
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  }
  if (request->body_source.content_length_known) {
    if (available == 0u) {
      if (produced == 0u) return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_EOF, 0u};
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "request-source-length";
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
    if (produced == 0u) {
      request->failure_status = SALTS_EPROTO;
      request->failure_stage = "request-source-length";
      return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
    }
  }
  if (request->source_transferred > request->max_request_body_bytes ||
      produced > request->max_request_body_bytes - request->source_transferred) {
    request->failure_status = SALTS_EMSGSIZE;
    request->failure_stage = "request-source-size";
    return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_ERROR, 0u};
  }
  request->source_transferred += produced;
  if (produced == 0u) return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_EOF, 0u};
  return (chttp_h2_proto_source_result){CHTTP_H2_PROTO_SOURCE_DATA, produced};
}

int chttp_h2_session_resume_file_source(chttp_h2_request_state *request) {
  chttp_h2_session *session;
  int status;
  if (request == NULL || request->session == NULL || request->completed || request->stream_id <= 0)
    return SALTS_ENOENT;
  session = request->session;
  if (chttp_h2_proto_resume_source(session->protocol, request->stream_id) != 0) {
    chttp_h2_session_fail(session, SALTS_EPROTO, 0, "h2-file-source-resume");
    return SALTS_EPROTO;
  }
  status = chttp_h2_session_flush(session);
  if (status != SALTS_OK) chttp_h2_session_fail(session, status, 0, "h2-file-source-resume");
  return status;
}

int chttp_h2_session_resume_file_sink(chttp_h2_request_state *request) {
  chttp_h2_session *session;
  chttp_file_sink_result sink_result;
  size_t flow_bytes;
  int native_status = 0;
  int result_status = SALTS_OK;
  int progress_status = SALTS_OK;
  if (request == NULL || request->session == NULL || request->completed ||
      request->stream_id <= 0 || request->file_sink_transfer == NULL ||
      !request->sink_write_pending)
    return SALTS_ENOENT;
  session = request->session;
  sink_result = chttp_file_sink_transfer_advance(request->file_sink_transfer);
  if (sink_result == CHTTP_FILE_SINK_WAIT) return SALTS_OK;
  flow_bytes = request->sink_flow_bytes;
  request->sink_flow_bytes = 0u;
  request->sink_write_pending = false;
  if (sink_result == CHTTP_FILE_SINK_ERROR) {
    request->failure_status =
        chttp_file_sink_transfer_status(request->file_sink_transfer, &native_status);
    if (request->failure_status == SALTS_OK) request->failure_status = SALTS_EIO;
    request->terminal_native_status = native_status;
    request->failure_stage = "file-write";
    result_status = request->failure_status;
    if (flow_bytes != 0u && chttp_h2_proto_consume_connection(session->protocol, flow_bytes) != 0)
      progress_status = SALTS_EPROTO;
    session->defer_completions = true;
    if (progress_status == SALTS_OK &&
        chttp_h2_proto_submit_rst_stream(session->protocol, request->stream_id,
                                         CHTTP_H2_ERR_CANCEL) != 0)
      progress_status = SALTS_EPROTO;
    if (progress_status == SALTS_OK &&
        chttp_h2_proto_resume_input(session->protocol, request->stream_id) != 0)
      progress_status = SALTS_EPROTO;
    session->defer_completions = false;
  } else {
    if (flow_bytes != 0u &&
        (chttp_h2_proto_consume_stream(session->protocol, request->stream_id, flow_bytes) != 0 ||
         chttp_h2_proto_consume_connection(session->protocol, flow_bytes) != 0))
      progress_status = SALTS_EPROTO;
    if (progress_status == SALTS_OK &&
        chttp_h2_proto_resume_input(session->protocol, request->stream_id) != 0)
      progress_status = SALTS_EPROTO;
  }
  if (progress_status == SALTS_OK && !chttp_h2_proto_input_paused(session->protocol))
    progress_status = chttp_h2_session_arm_receive(session);
  if (progress_status == SALTS_OK) progress_status = chttp_h2_session_flush(session);
  if (progress_status != SALTS_OK) {
    chttp_h2_session_fail(session, progress_status, 0, "h2-file-sink-resume");
    return progress_status;
  }
  return result_status;
}

int chttp_h2_session_submit(chttp_h2_session *session, chttp_h2_request_state *request,
                            const chttp_request_options *options) {
  chttp_h2_hpack_header *headers = NULL;
  char *header_names = NULL;
  size_t header_count = 0u;
  size_t index;
  int32_t stream_id = 0;
  int status;
  if (session == NULL || request == NULL || options == NULL || request->completed ||
      (session->state != CHTTP_H2_SESSION_CONNECTING && session->state != CHTTP_H2_SESSION_ACTIVE))
    return SALTS_ESHUTDOWN;
  if (session->active_requests >= session->request_capacity) return SALTS_EBUSY;
  for (index = 0u; index < session->request_capacity; ++index)
    if (session->requests[index] == NULL) break;
  if (index == session->request_capacity) return SALTS_EBUSY;
  status = chttp_h2_headers_build(options, &headers, &header_names, &header_count);
  if (status != SALTS_OK) return status;
  if (chttp_h2_proto_submit_request_ex(
          session->protocol, headers, header_count, request->request_body, options->body_size,
          request->source_enabled ? chttp_h2_request_source : NULL,
          request->source_enabled ? request : NULL, 0u, 0, 0, &stream_id) != 0) {
    free(header_names);
    free(headers);
    return SALTS_EBUSY;
  }
  free(header_names);
  free(headers);
  request->session = session;
  request->stream_id = stream_id;
  request->registry_index = index;
  session->requests[index] = request;
  ++session->active_requests;
  if (chttp_h2_proto_set_stream_user_data(session->protocol, stream_id, request) != 0) {
    session->requests[index] = NULL;
    --session->active_requests;
    request->session = NULL;
    (void)chttp_h2_proto_submit_rst_stream(session->protocol, stream_id,
                                           CHTTP_H2_ERR_INTERNAL_ERROR);
    return SALTS_EPROTO;
  }
  return SALTS_OK;
}

int chttp_h2_session_cancel(chttp_h2_session *session, chttp_h2_request_state *request) {
  int status;
  if (session == NULL || request == NULL || request->session != session || request->completed)
    return SALTS_ENOENT;
  session->defer_completions = true;
  status = chttp_h2_proto_submit_rst_stream(session->protocol, request->stream_id,
                                            CHTTP_H2_ERR_CANCEL) == 0
               ? SALTS_OK
               : SALTS_EPROTO;
  if (status == SALTS_OK &&
      chttp_h2_proto_resume_input(session->protocol, request->stream_id) == 0 &&
      !chttp_h2_proto_input_paused(session->protocol)) {
    const int receive_status = chttp_h2_session_arm_receive(session);
    if (receive_status != SALTS_OK) status = receive_status;
  }
  session->defer_completions = false;
  return status;
}

int chttp_h2_session_progress(chttp_h2_session *session) {
  size_t index;
  int status;
  if (session == NULL || session->state == CHTTP_H2_SESSION_FREE ||
      session->state == CHTTP_H2_SESSION_TERMINAL)
    return SALTS_OK;
  for (index = 0u; index < session->request_capacity; ++index) {
    chttp_h2_request_state *request = session->requests[index];
    if (request != NULL && request->terminal_pending) {
      const int terminal_status = request->terminal_status;
      const int terminal_native_status = request->terminal_native_status;
      const char *terminal_stage = request->terminal_stage;
      request->terminal_pending = false;
      chttp_h2_session_complete(session, request, NULL, terminal_status, terminal_native_status,
                                terminal_stage);
    }
  }
  if (session->close_pending) {
    status = chttp_h2_session_try_close(session);
    if (status != SALTS_OK) return status;
  }
  return chttp_h2_session_flush(session);
}

int chttp_h2_session_begin_stop(chttp_h2_session *session) {
  if (session == NULL || session->state == CHTTP_H2_SESSION_FREE ||
      session->state == CHTTP_H2_SESSION_TERMINAL || session->state == CHTTP_H2_SESSION_CLOSING)
    return SALTS_OK;
  if (session->state == CHTTP_H2_SESSION_CONNECTING) return chttp_h2_session_try_close(session);
  if (session->state != CHTTP_H2_SESSION_DRAINING) {
    if (chttp_h2_proto_submit_goaway(session->protocol,
                                     chttp_h2_proto_get_last_proc_stream_id(session->protocol),
                                     CHTTP_H2_ERR_NO_ERROR) != 0)
      return SALTS_EPROTO;
    session->state = CHTTP_H2_SESSION_DRAINING;
  }
  if (session->active_requests == 0u) session->close_after_flush = true;
  return SALTS_OK;
}

bool chttp_h2_session_stop_ready(const chttp_h2_session *session) {
  if (session == NULL || session->state == CHTTP_H2_SESSION_FREE ||
      session->state == CHTTP_H2_SESSION_TERMINAL || session->state == CHTTP_H2_SESSION_CLOSING)
    return true;
  return session->active_requests == 0u && !session->send_active &&
         session->pending_output_size == 0u && !chttp_h2_proto_want_write(session->protocol);
}

void chttp_h2_session_destroy(chttp_h2_session *session) {
  if (session == NULL) return;
  chttp_h2_proto_destroy(session->protocol);
  chttp_tls_profile_release(session->tls_profile);
  free(session->pending_output);
  free(session->requests);
  free(session->authority);
  free(session->connection_uri);
  memset(session, 0, sizeof(*session));
}
