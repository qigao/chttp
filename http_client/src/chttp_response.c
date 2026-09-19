#include "chttp_internal.h"

#include "chttp_file_sink.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_RESPONSE_LINE_OVERHEAD_BYTES = 15 };

static bool chttp_response_size_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || left > SIZE_MAX - right) return false;
  *out = left + right;
  return true;
}

static bool chttp_response_size_mul(size_t left, size_t right, size_t *out) {
  if (out == NULL || (left != 0u && right > SIZE_MAX / left)) return false;
  *out = left * right;
  return true;
}

static chttp_response_parser *chttp_parser_context(llhttp_t *parser) {
  return parser != NULL ? (chttp_response_parser *)parser->data : NULL;
}

static void chttp_response_set_failure(chttp_response_parser *parser, int status, const char *stage,
                                       const char *reason) {
  if (parser == NULL || parser->failure_status != SALTS_OK) return;
  parser->failure_status = status;
  parser->failure_stage = stage;
  llhttp_set_error_reason(&parser->parser, reason);
}

static int chttp_response_fail_data(chttp_response_parser *parser, int status, const char *stage,
                                    const char *reason) {
  chttp_response_set_failure(parser, status, stage, reason);
  return HPE_USER;
}

static int chttp_response_fail_callback(chttp_response_parser *parser, int status,
                                        const char *stage, const char *reason) {
  chttp_response_set_failure(parser, status, stage, reason);
  return -1;
}

static bool chttp_response_storage_append(chttp_response_parser *parser, const char *data,
                                          size_t size) {
  if (parser->header_storage_used > parser->header_storage_capacity ||
      size > parser->header_storage_capacity - parser->header_storage_used)
    return false;
  if (size != 0u) memcpy(parser->header_storage + parser->header_storage_used, data, size);
  parser->header_storage_used += size;
  return true;
}

static bool chttp_response_wire_add(chttp_response_parser *parser, size_t size) {
  if (parser->header_wire_bytes > parser->max_header_bytes ||
      size > parser->max_header_bytes - parser->header_wire_bytes)
    return false;
  parser->header_wire_bytes += size;
  return true;
}

static void chttp_response_reset_message(chttp_response_parser *parser) {
  parser->response =
      (chttp_response_view){.headers = parser->headers,
                            .reason = parser->reason_storage,
                            .body = parser->body_sink_enabled ? NULL : parser->body_storage};
  parser->header_storage_used = 0u;
  parser->header_wire_bytes = 0u;
  parser->field_offset = 0u;
  parser->value_offset = 0u;
  parser->reason_size = 0u;
  parser->current_field = (vstr){0};
  parser->current_value = (vstr){0};
  parser->reason_view = (vstr){0};
  parser->field_open = false;
  parser->value_open = false;
  parser->reason_terminated = false;
  if (parser->reason_storage != NULL) parser->reason_storage[0] = '\0';
}

static int chttp_response_on_message_begin(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL) return -1;
  chttp_response_reset_message(parser);
  return 0;
}

static int chttp_response_on_status(llhttp_t *llparser, const char *at, size_t length) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->reason_terminated || parser->reason_size > parser->max_reason_bytes ||
      length > parser->max_reason_bytes - parser->reason_size)
    return chttp_response_fail_data(parser, SALTS_EMSGSIZE, "status-line",
                                    "HTTP response status line exceeds configured bound");
  if (length != 0u) memcpy(parser->reason_storage + parser->reason_size, at, length);
  parser->reason_size += length;
  return 0;
}

static int chttp_response_on_status_complete(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL || parser->reason_size > parser->max_reason_bytes) return -1;
  parser->reason_view = vstr_from_buf(parser->reason_storage, parser->reason_size);
  parser->reason_storage[parser->reason_size] = '\0';
  parser->response.reason = parser->reason_view.data;
  parser->reason_terminated = true;
  return 0;
}

static int chttp_response_on_header_field(llhttp_t *llparser, const char *at, size_t length) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->value_open)
    return chttp_response_fail_data(parser, SALTS_EPROTO, "headers",
                                    "header field started before value completion");
  if (!parser->field_open) {
    parser->field_offset = parser->header_storage_used;
    parser->field_open = true;
  }
  if (!chttp_response_wire_add(parser, length) ||
      !chttp_response_storage_append(parser, at, length))
    return chttp_response_fail_data(parser, SALTS_EMSGSIZE, "headers",
                                    "HTTP response headers exceed configured byte bound");
  return 0;
}

static int chttp_response_on_header_field_complete(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  size_t field_size;
  if (parser == NULL || !parser->field_open ||
      parser->response.header_count >= parser->max_header_count)
    return chttp_response_fail_callback(parser, SALTS_EMSGSIZE, "headers",
                                        "HTTP response header count exceeds configured bound");
  if (parser->header_storage_used < parser->field_offset)
    return chttp_response_fail_callback(parser, SALTS_EPROTO, "headers",
                                        "HTTP response header field offsets are invalid");
  field_size = parser->header_storage_used - parser->field_offset;
  if (!chttp_response_storage_append(parser, "\0", 1u))
    return chttp_response_fail_callback(parser, SALTS_EMSGSIZE, "headers",
                                        "HTTP response header storage exhausted");
  parser->current_field =
      vstr_from_buf(parser->header_storage + parser->field_offset, field_size);
  parser->field_open = false;
  parser->value_offset = parser->header_storage_used;
  parser->value_open = true;
  return 0;
}

static int chttp_response_on_header_value(llhttp_t *llparser, const char *at, size_t length) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (!parser->value_open)
    return chttp_response_fail_data(parser, SALTS_EPROTO, "headers",
                                    "header value arrived without a field");
  if (!chttp_response_wire_add(parser, length) ||
      !chttp_response_storage_append(parser, at, length))
    return chttp_response_fail_data(parser, SALTS_EMSGSIZE, "headers",
                                    "HTTP response headers exceed configured byte bound");
  return 0;
}

static int chttp_response_on_header_value_complete(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  chttp_header *header;
  size_t value_size;
  if (parser == NULL || !parser->value_open ||
      parser->response.header_count >= parser->max_header_count)
    return chttp_response_fail_callback(parser, SALTS_EMSGSIZE, "headers",
                                        "HTTP response header count exceeds configured bound");
  if (parser->header_storage_used < parser->value_offset)
    return chttp_response_fail_callback(parser, SALTS_EPROTO, "headers",
                                        "HTTP response header value offsets are invalid");
  value_size = parser->header_storage_used - parser->value_offset;
  if (!chttp_response_wire_add(parser, 4u) || !chttp_response_storage_append(parser, "\0", 1u))
    return chttp_response_fail_callback(parser, SALTS_EMSGSIZE, "headers",
                                        "HTTP response headers exceed configured byte bound");
  parser->current_value =
      vstr_from_buf(parser->header_storage + parser->value_offset, value_size);
  header = &parser->headers[parser->response.header_count++];
  header->name = parser->current_field.data;
  header->value = parser->current_value.data;
  parser->current_field = (vstr){0};
  parser->current_value = (vstr){0};
  parser->value_open = false;
  return 0;
}

static int chttp_response_on_headers_complete(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  const unsigned int major = (unsigned int)llhttp_get_http_major(llparser);
  const unsigned int minor = (unsigned int)llhttp_get_http_minor(llparser);
  const unsigned int status = (unsigned int)llhttp_get_status_code(llparser);
  if (parser == NULL) return -1;
  if (parser->field_open || parser->value_open)
    return chttp_response_fail_callback(parser, SALTS_EPROTO, "headers",
                                        "HTTP response ended with an incomplete header");
  if (!parser->reason_terminated) {
    parser->reason_view = vstr_from_buf(parser->reason_storage, parser->reason_size);
    parser->reason_storage[parser->reason_size] = '\0';
    parser->response.reason = parser->reason_view.data;
    parser->reason_terminated = true;
  }
  parser->response.http_major = major;
  parser->response.http_minor = minor;
  parser->response.status_code = status;
  if (major != 1u)
    return chttp_response_fail_callback(parser, SALTS_ENOTSUP, "version",
                                        "only HTTP/1.x responses are supported");
  if (status == 101u || llhttp_get_upgrade(llparser) != 0u)
    return chttp_response_fail_callback(parser, SALTS_ENOTSUP, "upgrade",
                                        "HTTP protocol upgrades are not supported");
  return parser->request_method == CHTTP_METHOD_HEAD ? 1 : 0;
}

static int chttp_response_on_body(llhttp_t *llparser, const char *at, size_t length) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  int status;
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->response.body_size > parser->max_response_body_bytes ||
      length > parser->max_response_body_bytes - parser->response.body_size)
    return chttp_response_fail_data(parser, SALTS_EMSGSIZE, "body",
                                    "HTTP response body exceeds configured bound");
  if (length != 0u && parser->file_sink_transfer != NULL) {
    status = chttp_file_sink_transfer_append(parser->file_sink_transfer, at, length);
    if (status != SALTS_OK)
      return chttp_response_fail_data(parser, status, "file-write-buffer",
                                      "HTTP file sink could not retain body bytes");
  } else if (length != 0u && parser->body_sink_enabled) {
    status = parser->body_sink.write(parser->body_sink.user, at, length);
    if (status != SALTS_OK)
      return chttp_response_fail_data(parser, status, "response-sink",
                                      "HTTP response sink rejected body bytes");
  } else if (length != 0u) {
    memcpy(parser->body_storage + parser->response.body_size, at, length);
  }
  parser->response.body_size += length;
  return 0;
}

static int chttp_response_on_message_complete(llhttp_t *llparser) {
  chttp_response_parser *parser = chttp_parser_context(llparser);
  if (parser == NULL) return -1;
  if (parser->response.status_code < 200u) {
    if (parser->informational_responses >= parser->max_informational_responses)
      return chttp_response_fail_callback(parser, SALTS_EMSGSIZE, "informational-response",
                                          "too many informational HTTP responses");
    ++parser->informational_responses;
    return 0;
  }
  parser->response.protocol_keep_alive = llhttp_should_keep_alive(llparser);
  parser->complete = true;
  return HPE_PAUSED;
}

int chttp_response_parser_init(chttp_response_parser *parser, chttp_method method,
                               const chttp_limits *limits) {
  return chttp_response_parser_init_with_sinks(parser, method, limits, NULL, NULL);
}

int chttp_response_parser_init_with_sink(chttp_response_parser *parser, chttp_method method,
                                         const chttp_limits *limits, const chttp_body_sink *sink) {
  return chttp_response_parser_init_with_sinks(parser, method, limits, sink, NULL);
}

int chttp_response_parser_init_with_sinks(chttp_response_parser *parser, chttp_method method,
                                          const chttp_limits *limits,
                                          const chttp_body_sink *sink,
                                          chttp_file_sink_transfer *file_sink_transfer) {
  size_t headers_bytes;
  size_t terminator_bytes;
  size_t header_storage_capacity;
  size_t max_reason_bytes;
  size_t reason_capacity;
  size_t body_capacity;
  size_t total_capacity;
  unsigned char *cursor;

  if (parser == NULL || limits == NULL ||
      limits->max_start_line_bytes <= CHTTP_RESPONSE_LINE_OVERHEAD_BYTES ||
      limits->max_header_count == 0u || limits->max_header_bytes == 0u ||
      limits->max_response_body_bytes == 0u || limits->max_informational_responses == 0u ||
      (sink != NULL && sink->write == NULL) || (sink != NULL && file_sink_transfer != NULL))
    return SALTS_EINVAL;

  memset(parser, 0, sizeof(*parser));
  max_reason_bytes = limits->max_start_line_bytes - CHTTP_RESPONSE_LINE_OVERHEAD_BYTES;
  if (!chttp_response_size_mul(limits->max_header_count, sizeof(chttp_header), &headers_bytes) ||
      !chttp_response_size_mul(limits->max_header_count, 2u, &terminator_bytes) ||
      !chttp_response_size_add(limits->max_header_bytes, terminator_bytes,
                               &header_storage_capacity) ||
      !chttp_response_size_add(header_storage_capacity, 1u, &header_storage_capacity) ||
      !chttp_response_size_add(max_reason_bytes, 1u, &reason_capacity))
    return SALTS_ERANGE;

  body_capacity = sink == NULL && file_sink_transfer == NULL ? limits->max_response_body_bytes : 0u;
  if (!chttp_response_size_add(headers_bytes, header_storage_capacity, &total_capacity) ||
      !chttp_response_size_add(total_capacity, reason_capacity, &total_capacity) ||
      !chttp_response_size_add(total_capacity, body_capacity, &total_capacity))
    return SALTS_ERANGE;

  parser->arena = (unsigned char *)calloc(1u, total_capacity);
  if (parser->arena == NULL) return SALTS_ENOMEM;
  parser->arena_capacity = total_capacity;

  cursor = parser->arena;
  parser->headers = (chttp_header *)cursor;
  cursor += headers_bytes;
  parser->header_storage = (char *)cursor;
  cursor += header_storage_capacity;
  parser->reason_storage = (char *)cursor;
  cursor += reason_capacity;
  parser->body_storage = body_capacity != 0u ? cursor : NULL;

  parser->header_storage_capacity = header_storage_capacity;
  parser->max_header_count = limits->max_header_count;
  parser->max_header_bytes = limits->max_header_bytes;
  parser->max_response_body_bytes = limits->max_response_body_bytes;
  parser->max_reason_bytes = max_reason_bytes;
  parser->max_informational_responses = limits->max_informational_responses;
  parser->request_method = method;
  parser->file_sink_transfer = file_sink_transfer;
  if (sink != NULL) {
    parser->body_sink = *sink;
    parser->body_sink_enabled = true;
  }

  llhttp_settings_init(&parser->settings);
  parser->settings.on_message_begin = chttp_response_on_message_begin;
  parser->settings.on_status = chttp_response_on_status;
  parser->settings.on_status_complete = chttp_response_on_status_complete;
  parser->settings.on_header_field = chttp_response_on_header_field;
  parser->settings.on_header_field_complete = chttp_response_on_header_field_complete;
  parser->settings.on_header_value = chttp_response_on_header_value;
  parser->settings.on_header_value_complete = chttp_response_on_header_value_complete;
  parser->settings.on_headers_complete = chttp_response_on_headers_complete;
  parser->settings.on_body = chttp_response_on_body;
  parser->settings.on_message_complete = chttp_response_on_message_complete;
  llhttp_init(&parser->parser, HTTP_RESPONSE, &parser->settings);
  parser->parser.data = parser;
  chttp_response_reset_message(parser);
  return SALTS_OK;
}

void chttp_response_parser_destroy(chttp_response_parser *parser) {
  if (parser == NULL) return;
  free(parser->arena);
  memset(parser, 0, sizeof(*parser));
}

int chttp_response_parser_execute(chttp_response_parser *parser, const void *data, size_t size) {
  llhttp_errno_t status;
  const char *error_position;
  if (parser == NULL || data == NULL || size == 0u) return SALTS_EINVAL;
  if (parser->complete) return SALTS_EALREADY;
  status = llhttp_execute(&parser->parser, (const char *)data, size);
  parser->parser_status = (int)status;
  if (status == HPE_OK) return SALTS_OK;
  if (status == HPE_PAUSED && parser->complete) {
    error_position = llhttp_get_error_pos(&parser->parser);
    if (error_position == (const char *)data + size) return SALTS_OK;
    parser->complete = false;
    parser->failure_status = SALTS_EPROTO;
    parser->failure_stage = "trailing-data";
    return SALTS_EPROTO;
  }
  if (parser->failure_status != SALTS_OK) return parser->failure_status;
  parser->failure_status = status == HPE_PAUSED_UPGRADE ? SALTS_ENOTSUP : SALTS_EPROTO;
  parser->failure_stage = status == HPE_PAUSED_UPGRADE ? "upgrade" : "parse";
  return parser->failure_status;
}

int chttp_response_parser_finish(chttp_response_parser *parser) {
  llhttp_errno_t status;
  if (parser == NULL) return SALTS_EINVAL;
  if (parser->complete) return SALTS_OK;
  status = llhttp_finish(&parser->parser);
  parser->parser_status = (int)status;
  if ((status == HPE_OK || status == HPE_PAUSED) && parser->complete) return SALTS_OK;
  if (parser->failure_status != SALTS_OK) return parser->failure_status;
  parser->failure_status = SALTS_EPROTO;
  parser->failure_stage = "eof";
  return SALTS_EPROTO;
}
