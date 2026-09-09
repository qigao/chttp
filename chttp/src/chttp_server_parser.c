#include "chttp_server_internal.h"

#include <llhttp.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_SERVER_REQUEST_LINE_OVERHEAD_BYTES = 32u };

typedef enum chttp_server_wire_phase {
  CHTTP_SERVER_WIRE_PREAMBLE = 0,
  CHTTP_SERVER_WIRE_START_LINE,
  CHTTP_SERVER_WIRE_HEADERS,
  CHTTP_SERVER_WIRE_HEADERS_PENDING,
  CHTTP_SERVER_WIRE_FIXED_BODY,
  CHTTP_SERVER_WIRE_CHUNK_LINE,
  CHTTP_SERVER_WIRE_CHUNK_LINE_PENDING,
  CHTTP_SERVER_WIRE_CHUNK_BODY,
  CHTTP_SERVER_WIRE_CHUNK_BODY_END,
  CHTTP_SERVER_WIRE_TRAILERS,
  CHTTP_SERVER_WIRE_TRAILERS_PENDING
} chttp_server_wire_phase;

typedef struct chttp_server_parser_impl {
  llhttp_t parser;
  llhttp_settings_t settings;
  chttp_server_request_view request;
  chttp_header *headers;
  char *target_storage;
  char *path_storage;
  char *header_storage;
  unsigned char *body_storage;
  size_t body_storage_capacity;
  size_t max_target_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_body_bytes;
  size_t max_start_line_wire_bytes;
  size_t target_size;
  size_t header_storage_capacity;
  size_t header_storage_used;
  size_t field_offset;
  size_t value_offset;
  size_t start_line_wire_bytes;
  size_t header_wire_bytes;
  size_t wire_line_bytes;
  size_t chunk_body_remaining;
  size_t chunk_body_end_bytes;
  unsigned int failure_http_status;
  int callback_status;
  chttp_server_parser_request_fn on_request;
  chttp_server_parser_headers_fn on_headers;
  chttp_server_parser_continue_fn on_continue;
  chttp_server_parser_body_open_fn on_body_open;
  chttp_server_parser_body_close_fn on_body_close;
  chttp_server_parser_upgrade_fn on_upgrade;
  chttp_server_parser_buffer_grow_fn buffer_grow;
  chttp_server_parser_buffer_release_fn buffer_release;
  void *buffer_context;
  chttp_body_sink body_sink;
  void *user;
  bool field_open;
  bool value_open;
  bool target_terminated;
  bool body_sink_open;
  bool terminal;
  bool paused;
  bool upgrade_stopped;
  bool headers_stopped;
  bool wire_previous_was_cr;
  chttp_server_wire_phase wire_phase;
} chttp_server_parser_impl;

static int chttp_server_ascii_equal(const char *left, const char *right) {
  unsigned char a;
  unsigned char b;
  if (left == NULL || right == NULL) return 0;
  do {
    a = (unsigned char)*left++;
    b = (unsigned char)*right++;
    if (a >= (unsigned char)'A' && a <= (unsigned char)'Z') a = (unsigned char)(a + 32u);
    if (b >= (unsigned char)'A' && b <= (unsigned char)'Z') b = (unsigned char)(b + 32u);
    if (a != b) return 0;
  } while (a != 0u);
  return 1;
}

const char *chttp_server_request_header(const chttp_server_request_view *request,
                                        const char *name) {
  size_t index;
  if (request == NULL || name == NULL) return NULL;
  for (index = 0u; index < request->header_count; ++index) {
    if (chttp_server_ascii_equal(request->headers[index].name, name))
      return request->headers[index].value;
  }
  return NULL;
}

static chttp_server_parser_impl *chttp_server_parser_context(llhttp_t *parser) {
  return parser == NULL ? NULL : (chttp_server_parser_impl *)parser->data;
}

static int chttp_server_parser_fail(chttp_server_parser_impl *parser, unsigned int http_status) {
  parser->failure_http_status = http_status;
  return HPE_USER;
}

static int chttp_server_parser_callback_fail(chttp_server_parser_impl *parser, int status) {
  parser->callback_status = status == SALTS_OK ? SALTS_EIO : status;
  return HPE_USER;
}

static void chttp_server_parser_close_body(chttp_server_parser_impl *parser, int status) {
  chttp_body_sink sink;
  if (parser == NULL || !parser->body_sink_open) return;
  sink = parser->body_sink;
  parser->body_sink_open = false;
  parser->body_sink = (chttp_body_sink){0};
  if (parser->on_body_close != NULL) parser->on_body_close(parser->user, &sink, status);
}

static bool chttp_server_parser_storage_append(chttp_server_parser_impl *parser, const char *data,
                                               size_t size) {
  if (parser->header_storage_used > parser->header_storage_capacity ||
      size > parser->header_storage_capacity - parser->header_storage_used)
    return false;
  if (size != 0u) memcpy(parser->header_storage + parser->header_storage_used, data, size);
  parser->header_storage_used += size;
  return true;
}

static void chttp_server_parser_reset_message(chttp_server_parser_impl *parser) {
  if (parser->body_storage != NULL) {
    if (parser->buffer_release != NULL)
      parser->buffer_release(parser->buffer_context, parser->body_storage,
                             parser->body_storage_capacity);
    else free(parser->body_storage);
    parser->body_storage = NULL;
    parser->body_storage_capacity = 0u;
  }
  parser->request = (chttp_server_request_view){.target = parser->target_storage,
                                                .path = parser->path_storage,
                                                .headers = parser->headers,
                                                .body = parser->body_storage};
  parser->target_size = 0u;
  parser->header_storage_used = 0u;
  parser->field_offset = 0u;
  parser->value_offset = 0u;
  parser->failure_http_status = 0u;
  parser->callback_status = SALTS_OK;
  parser->field_open = false;
  parser->value_open = false;
  parser->target_terminated = false;
  parser->target_storage[0] = '\0';
  parser->path_storage[0] = '\0';
}

static void chttp_server_parser_reset_wire(chttp_server_parser_impl *parser) {
  parser->start_line_wire_bytes = 0u;
  parser->header_wire_bytes = 0u;
  parser->wire_line_bytes = 0u;
  parser->chunk_body_remaining = 0u;
  parser->chunk_body_end_bytes = 0u;
  parser->wire_previous_was_cr = false;
  parser->wire_phase = CHTTP_SERVER_WIRE_PREAMBLE;
}

static int chttp_server_parser_on_message_begin(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL) return HPE_USER;
  chttp_server_parser_reset_message(parser);
  return 0;
}

static int chttp_server_parser_on_url(llhttp_t *llparser, const char *at, size_t length) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->target_terminated || parser->target_size > parser->max_target_bytes ||
      length > parser->max_target_bytes - parser->target_size)
    return chttp_server_parser_fail(parser, 414u);
  if (length != 0u) memcpy(parser->target_storage + parser->target_size, at, length);
  parser->target_size += length;
  return 0;
}

static int chttp_server_parser_on_url_complete(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL || parser->target_size == 0u || parser->target_size > parser->max_target_bytes)
    return chttp_server_parser_fail(parser, 400u);
  parser->target_storage[parser->target_size] = '\0';
  parser->target_terminated = true;
  return 0;
}

static int chttp_server_parser_on_header_field(llhttp_t *llparser, const char *at, size_t length) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->wire_phase == CHTTP_SERVER_WIRE_TRAILERS ||
      parser->wire_phase == CHTTP_SERVER_WIRE_TRAILERS_PENDING)
    return chttp_server_parser_fail(parser, 400u);
  if (parser->value_open) return chttp_server_parser_fail(parser, 400u);
  if (!parser->field_open) {
    parser->field_offset = parser->header_storage_used;
    parser->field_open = true;
  }
  if (!chttp_server_parser_storage_append(parser, at, length))
    return chttp_server_parser_fail(parser, 431u);
  return 0;
}

static int chttp_server_parser_on_header_field_complete(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL || !parser->field_open ||
      parser->request.header_count >= parser->max_header_count)
    return chttp_server_parser_fail(parser, 431u);
  if (!chttp_server_parser_storage_append(parser, "\0", 1u))
    return chttp_server_parser_fail(parser, 431u);
  parser->field_open = false;
  parser->value_offset = parser->header_storage_used;
  parser->value_open = true;
  return 0;
}

static int chttp_server_parser_on_header_value(llhttp_t *llparser, const char *at, size_t length) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (!parser->value_open) return chttp_server_parser_fail(parser, 400u);
  if (!chttp_server_parser_storage_append(parser, at, length))
    return chttp_server_parser_fail(parser, 431u);
  return 0;
}

static int chttp_server_parser_on_header_value_complete(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  chttp_header *header;
  if (parser == NULL || !parser->value_open ||
      parser->request.header_count >= parser->max_header_count)
    return chttp_server_parser_fail(parser, 431u);
  if (!chttp_server_parser_storage_append(parser, "\0", 1u))
    return chttp_server_parser_fail(parser, 431u);
  header = &parser->headers[parser->request.header_count++];
  header->name = parser->header_storage + parser->field_offset;
  header->value = parser->header_storage + parser->value_offset;
  parser->value_open = false;
  return 0;
}

static chttp_method chttp_server_parser_method(const llhttp_t *parser) {
  switch ((llhttp_method_t)llhttp_get_method((llhttp_t *)parser)) {
  case HTTP_GET:
    return CHTTP_METHOD_GET;
  case HTTP_HEAD:
    return CHTTP_METHOD_HEAD;
  case HTTP_POST:
    return CHTTP_METHOD_POST;
  case HTTP_PUT:
    return CHTTP_METHOD_PUT;
  case HTTP_DELETE:
    return CHTTP_METHOD_DELETE;
  case HTTP_PATCH:
    return CHTTP_METHOD_PATCH;
  case HTTP_OPTIONS:
    return CHTTP_METHOD_OPTIONS;
  default:
    return (chttp_method)0;
  }
}

static const char *chttp_server_trim_ows(const char *value, size_t *out_size) {
  const char *end;
  if (value == NULL || out_size == NULL) return NULL;
  while (*value == ' ' || *value == '\t')
    ++value;
  end = value + strlen(value);
  while (end != value && (end[-1] == ' ' || end[-1] == '\t'))
    --end;
  *out_size = (size_t)(end - value);
  return value;
}

static bool chttp_server_ascii_equal_size(const char *value, size_t value_size,
                                          const char *expected) {
  size_t index;
  if (value == NULL || expected == NULL || strlen(expected) != value_size) return false;
  for (index = 0u; index < value_size; ++index) {
    unsigned char actual = (unsigned char)value[index];
    unsigned char wanted = (unsigned char)expected[index];
    if (actual >= (unsigned char)'A' && actual <= (unsigned char)'Z') actual += 32u;
    if (wanted >= (unsigned char)'A' && wanted <= (unsigned char)'Z') wanted += 32u;
    if (actual != wanted) return false;
  }
  return true;
}

static int chttp_server_parser_validate_headers(chttp_server_parser_impl *parser,
                                                bool *out_wants_continue) {
  const char *expect = NULL;
  const char *transfer_encoding = NULL;
  size_t expect_size = 0u;
  size_t transfer_encoding_size = 0u;
  size_t host_count = 0u;
  size_t index;
  *out_wants_continue = false;
  for (index = 0u; index < parser->request.header_count; ++index) {
    const chttp_header *header = &parser->request.headers[index];
    if (chttp_server_ascii_equal(header->name, "host")) {
      size_t host_size = 0u;
      (void)chttp_server_trim_ows(header->value, &host_size);
      if (++host_count > 1u || host_size == 0u) return chttp_server_parser_fail(parser, 400u);
    } else if (parser->request.http_minor == 1u &&
               chttp_server_ascii_equal(header->name, "expect")) {
      if (expect != NULL) return chttp_server_parser_fail(parser, 417u);
      expect = chttp_server_trim_ows(header->value, &expect_size);
    } else if (chttp_server_ascii_equal(header->name, "transfer-encoding")) {
      if (transfer_encoding != NULL) return chttp_server_parser_fail(parser, 501u);
      transfer_encoding = chttp_server_trim_ows(header->value, &transfer_encoding_size);
    }
  }
  if (parser->request.http_major == 1u && parser->request.http_minor == 1u && host_count != 1u)
    return chttp_server_parser_fail(parser, 400u);
  if (transfer_encoding != NULL) {
    if (parser->request.http_minor == 0u) return chttp_server_parser_fail(parser, 400u);
    if (!chttp_server_ascii_equal_size(transfer_encoding, transfer_encoding_size, "chunked") ||
        (parser->parser.flags & F_CHUNKED) == 0u)
      return chttp_server_parser_fail(parser, 501u);
  }
  if (expect != NULL) {
    static const char continue_value[] = "100-continue";
    size_t value_index;
    if (expect_size != sizeof(continue_value) - 1u) return chttp_server_parser_fail(parser, 417u);
    for (value_index = 0u; value_index < expect_size; ++value_index) {
      unsigned char actual = (unsigned char)expect[value_index];
      if (actual >= (unsigned char)'A' && actual <= (unsigned char)'Z') actual += 32u;
      if (actual != (unsigned char)continue_value[value_index])
        return chttp_server_parser_fail(parser, 417u);
    }
    *out_wants_continue = true;
  }
  return 0;
}

static int chttp_server_parser_on_headers_complete(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  size_t path_size;
  const char *query;
  bool wants_continue = false;
  int status;
  if (parser == NULL || parser->field_open || parser->value_open || !parser->target_terminated)
    return chttp_server_parser_fail(parser, 400u);
  parser->request.http_major = (unsigned int)llhttp_get_http_major(llparser);
  parser->request.http_minor = (unsigned int)llhttp_get_http_minor(llparser);
  if (parser->request.http_major != 1u || parser->request.http_minor > 1u)
    return chttp_server_parser_fail(parser, 505u);
  parser->request.method = chttp_server_parser_method(llparser);
  if (parser->request.method == (chttp_method)0) return chttp_server_parser_fail(parser, 501u);
  if (parser->target_storage[0] != '/' &&
      !(parser->request.method == CHTTP_METHOD_OPTIONS && parser->target_size == 1u &&
        parser->target_storage[0] == '*'))
    return chttp_server_parser_fail(parser, 400u);
  if (memchr(parser->target_storage, '#', parser->target_size) != NULL)
    return chttp_server_parser_fail(parser, 400u);
  query = (const char *)memchr(parser->target_storage, '?', parser->target_size);
  path_size = query == NULL ? parser->target_size : (size_t)(query - parser->target_storage);
  if (path_size == 0u) return chttp_server_parser_fail(parser, 400u);
  memcpy(parser->path_storage, parser->target_storage, path_size);
  parser->path_storage[path_size] = '\0';
  if ((llparser->flags & F_CONTENT_LENGTH) != 0u &&
      llparser->content_length > (uint64_t)parser->max_body_bytes)
    return chttp_server_parser_fail(parser, 413u);
  status = chttp_server_parser_validate_headers(parser, &wants_continue);
  if (status == 0 && parser->on_headers != NULL) {
    chttp_server_parser_headers_action action = CHTTP_SERVER_HEADERS_CONTINUE;
    status = parser->on_headers(parser->user, &parser->request, &action);
    if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
    if (action == CHTTP_SERVER_HEADERS_STOP) {
      parser->headers_stopped = true;
      return HPE_PAUSED;
    }
    if (action != CHTTP_SERVER_HEADERS_CONTINUE)
      return chttp_server_parser_callback_fail(parser, SALTS_EPROTO);
  }
  if (status == 0 && wants_continue) {
    if (parser->on_continue == NULL) return chttp_server_parser_fail(parser, 417u);
    status = parser->on_continue(parser->user);
    if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
  }
  if (status == 0 && llhttp_get_upgrade(llparser) != 0u) {
    if (parser->on_upgrade != NULL) {
      chttp_server_parser_upgrade_action action = CHTTP_SERVER_UPGRADE_IGNORE;
      unsigned int http_status = 0u;
      parser->request.protocol_keep_alive = 1;
      status = parser->on_upgrade(parser->user, &parser->request, &action, &http_status);
      if (status != SALTS_OK) {
        parser->failure_http_status = http_status;
        return chttp_server_parser_callback_fail(parser, status);
      }
      if (action == CHTTP_SERVER_UPGRADE_STOP) {
        parser->failure_http_status = http_status;
        parser->upgrade_stopped = true;
        return 2;
      }
      if (action != CHTTP_SERVER_UPGRADE_IGNORE)
        return chttp_server_parser_callback_fail(parser, SALTS_EPROTO);
    }
    llparser->upgrade = 0u;
  }
  if (status == 0) {
    if (parser->on_body_open != NULL) {
      chttp_body_sink sink = {0};
      status = parser->on_body_open(parser->user, &parser->request, &sink);
      if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
      if (sink.write != NULL) {
        parser->body_sink = sink;
        parser->body_sink_open = true;
        parser->request.body = NULL;
        parser->request.body_streamed = 1;
      }
    }
  }
  if (status == 0) {
    if ((llparser->flags & F_CHUNKED) != 0u) {
      parser->wire_line_bytes = 0u;
      parser->wire_previous_was_cr = false;
      parser->wire_phase = CHTTP_SERVER_WIRE_CHUNK_LINE;
    } else if ((llparser->flags & F_CONTENT_LENGTH) != 0u && llparser->content_length != 0u) {
      parser->chunk_body_remaining = (size_t)llparser->content_length;
      parser->wire_phase = CHTTP_SERVER_WIRE_FIXED_BODY;
    }
  }
  return status;
}

static int chttp_server_parser_on_body(llhttp_t *llparser, const char *at, size_t length) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  size_t required;
  if (parser == NULL || (length != 0u && at == NULL)) return HPE_USER;
  if (parser->request.body_size > parser->max_body_bytes ||
      length > parser->max_body_bytes - parser->request.body_size)
    return chttp_server_parser_fail(parser, 413u);
  required = parser->request.body_size + length;
  if (length != 0u && parser->body_sink_open) {
    const int status = parser->body_sink.write(parser->body_sink.user, at, length);
    if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
  } else if (length != 0u) {
    int status;
    if (parser->buffer_grow != NULL)
      status = parser->buffer_grow(parser->buffer_context, &parser->body_storage,
                                   &parser->body_storage_capacity, required,
                                   parser->max_body_bytes, parser->request.body_size);
    else {
      unsigned char *grown = (unsigned char *)realloc(parser->body_storage, required);
      status = grown == NULL ? SALTS_ENOMEM : SALTS_OK;
      if (grown != NULL) {
        parser->body_storage = grown;
        parser->body_storage_capacity = required;
      }
    }
    if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
    parser->request.body = parser->body_storage;
    memcpy(parser->body_storage + parser->request.body_size, at, length);
  }
  parser->request.body_size += length;
  return 0;
}

static int chttp_server_parser_on_message_complete(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  int status;
  if (parser == NULL) return HPE_USER;
  chttp_server_parser_reset_wire(parser);
  parser->request.protocol_keep_alive = llhttp_should_keep_alive(llparser);
  chttp_server_parser_close_body(parser, SALTS_OK);
  if (parser->upgrade_stopped) return 0;
  status = parser->on_request(parser->user, &parser->request);
  if (status == CHTTP_SERVER_REQUEST_DEFERRED) {
    parser->paused = true;
    return 0;
  }
  if (status != SALTS_OK) return chttp_server_parser_callback_fail(parser, status);
  return 0;
}

static int chttp_server_parser_on_chunk_header(llhttp_t *llparser) {
  chttp_server_parser_impl *parser = chttp_server_parser_context(llparser);
  if (parser == NULL) return HPE_USER;
  if (parser->request.body_size > parser->max_body_bytes ||
      llparser->content_length > (uint64_t)(parser->max_body_bytes - parser->request.body_size))
    return chttp_server_parser_fail(parser, 413u);
  parser->chunk_body_remaining = (size_t)llparser->content_length;
  if (parser->chunk_body_remaining == 0u) {
    parser->header_wire_bytes = 0u;
    parser->wire_line_bytes = 0u;
    parser->wire_previous_was_cr = false;
    parser->wire_phase = CHTTP_SERVER_WIRE_TRAILERS;
  } else {
    parser->wire_phase = CHTTP_SERVER_WIRE_CHUNK_BODY;
  }
  return 0;
}

int chttp_server_parser_init(chttp_server_parser *parser,
                             const chttp_server_parser_config *config) {
  chttp_server_parser_impl *impl;
  size_t storage_capacity;
  if (parser == NULL || config == NULL || parser->impl != NULL || config->max_target_bytes == 0u ||
      config->max_header_count == 0u || config->max_header_bytes == 0u ||
      config->max_body_bytes == 0u || config->on_request == NULL ||
      ((config->on_body_open == NULL) != (config->on_body_close == NULL)))
    return SALTS_EINVAL;
  if (config->max_target_bytes > SIZE_MAX - CHTTP_SERVER_REQUEST_LINE_OVERHEAD_BYTES ||
      config->max_header_bytes == SIZE_MAX ||
      config->max_header_count > SIZE_MAX / sizeof(chttp_header) ||
      config->max_header_count > (SIZE_MAX - config->max_header_bytes - 1u) / 2u)
    return SALTS_ERANGE;
  storage_capacity = config->max_header_bytes + config->max_header_count * 2u + 1u;
  impl = (chttp_server_parser_impl *)calloc(1u, sizeof(*impl));
  if (impl == NULL) return SALTS_ENOMEM;
  impl->headers = (chttp_header *)calloc(config->max_header_count, sizeof(*impl->headers));
  impl->target_storage = (char *)malloc(config->max_target_bytes + 1u);
  impl->path_storage = (char *)malloc(config->max_target_bytes + 1u);
  impl->header_storage = (char *)malloc(storage_capacity);
  if (impl->headers == NULL || impl->target_storage == NULL || impl->path_storage == NULL ||
      impl->header_storage == NULL) {
    free(impl->header_storage);
    free(impl->path_storage);
    free(impl->target_storage);
    free(impl->headers);
    free(impl);
    return SALTS_ENOMEM;
  }
  impl->max_target_bytes = config->max_target_bytes;
  impl->max_header_count = config->max_header_count;
  impl->max_header_bytes = config->max_header_bytes;
  impl->max_body_bytes = config->max_body_bytes;
  impl->max_start_line_wire_bytes =
      config->max_target_bytes + CHTTP_SERVER_REQUEST_LINE_OVERHEAD_BYTES;
  impl->header_storage_capacity = storage_capacity;
  impl->on_request = config->on_request;
  impl->on_headers = config->on_headers;
  impl->on_continue = config->on_continue;
  impl->on_body_open = config->on_body_open;
  impl->on_body_close = config->on_body_close;
  impl->on_upgrade = config->on_upgrade;
  impl->buffer_grow = config->buffer_grow;
  impl->buffer_release = config->buffer_release;
  impl->buffer_context = config->buffer_context;
  impl->user = config->user;
  llhttp_settings_init(&impl->settings);
  impl->settings.on_message_begin = chttp_server_parser_on_message_begin;
  impl->settings.on_url = chttp_server_parser_on_url;
  impl->settings.on_url_complete = chttp_server_parser_on_url_complete;
  impl->settings.on_header_field = chttp_server_parser_on_header_field;
  impl->settings.on_header_field_complete = chttp_server_parser_on_header_field_complete;
  impl->settings.on_header_value = chttp_server_parser_on_header_value;
  impl->settings.on_header_value_complete = chttp_server_parser_on_header_value_complete;
  impl->settings.on_headers_complete = chttp_server_parser_on_headers_complete;
  impl->settings.on_body = chttp_server_parser_on_body;
  impl->settings.on_message_complete = chttp_server_parser_on_message_complete;
  impl->settings.on_chunk_header = chttp_server_parser_on_chunk_header;
  llhttp_init(&impl->parser, HTTP_REQUEST, &impl->settings);
  impl->parser.data = impl;
  chttp_server_parser_reset_message(impl);
  chttp_server_parser_reset_wire(impl);
  parser->impl = impl;
  return SALTS_OK;
}

static int chttp_server_parser_execute_llhttp(chttp_server_parser_impl *parser, const char *data,
                                              size_t size, unsigned int *out_http_status) {
  const llhttp_errno_t status = llhttp_execute(&parser->parser, data, size);
  if (status == HPE_OK) return SALTS_OK;
  if (status == HPE_PAUSED_UPGRADE && parser->upgrade_stopped) return SALTS_OK;
  if (status == HPE_PAUSED && parser->headers_stopped) return SALTS_OK;
  parser->terminal = true;
  if (parser->callback_status != SALTS_OK) {
    chttp_server_parser_close_body(parser, parser->callback_status);
    return parser->callback_status;
  }
  chttp_server_parser_close_body(parser, SALTS_EPROTO);
  if (parser->failure_http_status == 0u)
    parser->failure_http_status = status == HPE_INVALID_VERSION ? 505u : 400u;
  *out_http_status = parser->failure_http_status;
  return SALTS_EPROTO;
}

static int chttp_server_parser_wire_fail(chttp_server_parser_impl *parser, unsigned int http_status,
                                         unsigned int *out_http_status) {
  parser->failure_http_status = http_status;
  parser->terminal = true;
  chttp_server_parser_close_body(parser, SALTS_EPROTO);
  *out_http_status = http_status;
  return SALTS_EPROTO;
}

static int chttp_server_parser_scan_head(chttp_server_parser_impl *parser, const char *data,
                                         size_t size, size_t *out_size,
                                         unsigned int *out_http_status) {
  size_t index;
  for (index = 0u; index < size; ++index) {
    const char byte = data[index];
    if (parser->wire_phase == CHTTP_SERVER_WIRE_PREAMBLE ||
        parser->wire_phase == CHTTP_SERVER_WIRE_START_LINE) {
      if (parser->start_line_wire_bytes >= parser->max_start_line_wire_bytes)
        return chttp_server_parser_wire_fail(parser, 400u, out_http_status);
      ++parser->start_line_wire_bytes;
      if (parser->wire_phase == CHTTP_SERVER_WIRE_PREAMBLE) {
        if (byte != '\r' && byte != '\n') {
          parser->wire_phase = CHTTP_SERVER_WIRE_START_LINE;
          parser->wire_line_bytes = 1u;
          parser->wire_previous_was_cr = byte == '\r';
        }
      } else {
        ++parser->wire_line_bytes;
        if (parser->wire_previous_was_cr && byte == '\n') {
          parser->wire_phase = CHTTP_SERVER_WIRE_HEADERS;
          parser->wire_line_bytes = 0u;
          parser->wire_previous_was_cr = false;
        } else {
          parser->wire_previous_was_cr = byte == '\r';
        }
      }
      continue;
    }

    if (parser->header_wire_bytes >= parser->max_header_bytes)
      return chttp_server_parser_wire_fail(parser, 431u, out_http_status);
    ++parser->header_wire_bytes;
    ++parser->wire_line_bytes;
    if (parser->wire_previous_was_cr && byte == '\n') {
      if (parser->wire_line_bytes == 2u) {
        parser->wire_phase = CHTTP_SERVER_WIRE_HEADERS_PENDING;
        *out_size = index + 1u;
        return SALTS_OK;
      }
      parser->wire_line_bytes = 0u;
      parser->wire_previous_was_cr = false;
    } else {
      parser->wire_previous_was_cr = byte == '\r';
    }
  }
  *out_size = size;
  return SALTS_OK;
}

static int chttp_server_parser_scan_line(chttp_server_parser_impl *parser, const char *data,
                                         size_t size, chttp_server_wire_phase pending_phase,
                                         unsigned int failure_status, size_t *out_size,
                                         unsigned int *out_http_status) {
  size_t index;
  for (index = 0u; index < size; ++index) {
    const char byte = data[index];
    if (parser->wire_line_bytes >= parser->max_header_bytes)
      return chttp_server_parser_wire_fail(parser, failure_status, out_http_status);
    ++parser->wire_line_bytes;
    if (parser->wire_previous_was_cr && byte == '\n') {
      parser->wire_phase = pending_phase;
      parser->wire_previous_was_cr = false;
      *out_size = index + 1u;
      return SALTS_OK;
    }
    parser->wire_previous_was_cr = byte == '\r';
  }
  *out_size = size;
  return SALTS_OK;
}

static int chttp_server_parser_scan_trailers(chttp_server_parser_impl *parser, const char *data,
                                             size_t size, size_t *out_size,
                                             unsigned int *out_http_status) {
  size_t index;
  for (index = 0u; index < size; ++index) {
    const char byte = data[index];
    if (parser->header_wire_bytes >= parser->max_header_bytes)
      return chttp_server_parser_wire_fail(parser, 431u, out_http_status);
    ++parser->header_wire_bytes;
    ++parser->wire_line_bytes;
    if (parser->wire_previous_was_cr && byte == '\n') {
      if (parser->wire_line_bytes == 2u) {
        parser->wire_phase = CHTTP_SERVER_WIRE_TRAILERS_PENDING;
        *out_size = index + 1u;
        return SALTS_OK;
      }
      parser->wire_line_bytes = 0u;
      parser->wire_previous_was_cr = false;
    } else {
      parser->wire_previous_was_cr = byte == '\r';
    }
  }
  *out_size = size;
  return SALTS_OK;
}

int chttp_server_parser_execute_consumed(chttp_server_parser *parser, const void *data, size_t size,
                                         size_t *out_consumed, unsigned int *out_http_status) {
  chttp_server_parser_impl *impl;
  const char *cursor = (const char *)data;
  size_t remaining = size;
  if (out_consumed != NULL) *out_consumed = 0u;
  if (out_http_status != NULL) *out_http_status = 0u;
  if (parser == NULL || parser->impl == NULL || data == NULL || size == 0u ||
      out_consumed == NULL || out_http_status == NULL)
    return SALTS_EINVAL;
  impl = (chttp_server_parser_impl *)parser->impl;
  if (impl->paused) return SALTS_EBUSY;
  if (impl->upgrade_stopped || impl->headers_stopped) return SALTS_ESHUTDOWN;
  if (impl->terminal) {
    *out_http_status = impl->failure_http_status;
    return impl->callback_status != SALTS_OK ? impl->callback_status : SALTS_EPROTO;
  }
  while (remaining != 0u) {
    size_t segment_size = remaining;
    int status = SALTS_OK;
    switch (impl->wire_phase) {
    case CHTTP_SERVER_WIRE_PREAMBLE:
    case CHTTP_SERVER_WIRE_START_LINE:
    case CHTTP_SERVER_WIRE_HEADERS:
      status =
          chttp_server_parser_scan_head(impl, cursor, remaining, &segment_size, out_http_status);
      break;
    case CHTTP_SERVER_WIRE_CHUNK_LINE:
      status = chttp_server_parser_scan_line(impl, cursor, remaining,
                                             CHTTP_SERVER_WIRE_CHUNK_LINE_PENDING, 413u,
                                             &segment_size, out_http_status);
      break;
    case CHTTP_SERVER_WIRE_FIXED_BODY:
    case CHTTP_SERVER_WIRE_CHUNK_BODY:
      if (segment_size > impl->chunk_body_remaining) segment_size = impl->chunk_body_remaining;
      impl->chunk_body_remaining -= segment_size;
      if (impl->chunk_body_remaining == 0u && impl->wire_phase == CHTTP_SERVER_WIRE_CHUNK_BODY) {
        impl->chunk_body_end_bytes = 0u;
        impl->wire_phase = CHTTP_SERVER_WIRE_CHUNK_BODY_END;
      }
      break;
    case CHTTP_SERVER_WIRE_CHUNK_BODY_END: {
      const size_t needed = 2u - impl->chunk_body_end_bytes;
      if (segment_size > needed) segment_size = needed;
      impl->chunk_body_end_bytes += segment_size;
      if (impl->chunk_body_end_bytes == 2u) {
        impl->wire_line_bytes = 0u;
        impl->wire_previous_was_cr = false;
        impl->wire_phase = CHTTP_SERVER_WIRE_CHUNK_LINE;
      }
      break;
    }
    case CHTTP_SERVER_WIRE_TRAILERS:
      status = chttp_server_parser_scan_trailers(impl, cursor, remaining, &segment_size,
                                                 out_http_status);
      break;
    case CHTTP_SERVER_WIRE_HEADERS_PENDING:
    case CHTTP_SERVER_WIRE_CHUNK_LINE_PENDING:
    case CHTTP_SERVER_WIRE_TRAILERS_PENDING:
      return chttp_server_parser_wire_fail(impl, 400u, out_http_status);
    }
    if (status != SALTS_OK) {
      *out_consumed = size - remaining;
      return status;
    }
    status = chttp_server_parser_execute_llhttp(impl, cursor, segment_size, out_http_status);
    if (status != SALTS_OK) {
      *out_consumed = size - remaining;
      return status;
    }
    cursor += segment_size;
    remaining -= segment_size;
    if (impl->upgrade_stopped || impl->headers_stopped || impl->paused) break;
  }
  *out_consumed = size - remaining;
  *out_http_status = impl->failure_http_status;
  return SALTS_OK;
}

int chttp_server_parser_execute(chttp_server_parser *parser, const void *data, size_t size,
                                unsigned int *out_http_status) {
  size_t consumed = 0u;
  const int status =
      chttp_server_parser_execute_consumed(parser, data, size, &consumed, out_http_status);
  return status == SALTS_OK && consumed != size ? SALTS_EBUSY : status;
}

int chttp_server_parser_resume(chttp_server_parser *parser) {
  chttp_server_parser_impl *impl;
  if (parser == NULL || parser->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_parser_impl *)parser->impl;
  if (!impl->paused) return SALTS_EALREADY;
  if (impl->terminal || impl->upgrade_stopped || impl->headers_stopped)
    return SALTS_ESHUTDOWN;
  impl->paused = false;
  return SALTS_OK;
}

int chttp_server_parser_reset(chttp_server_parser *parser) {
  chttp_server_parser_impl *impl;
  if (parser == NULL || parser->impl == NULL) return SALTS_EINVAL;
  impl = (chttp_server_parser_impl *)parser->impl;
  chttp_server_parser_close_body(impl, SALTS_ECANCELED);
  llhttp_reset(&impl->parser);
  impl->parser.data = impl;
  impl->terminal = false;
  impl->paused = false;
  impl->upgrade_stopped = false;
  impl->headers_stopped = false;
  chttp_server_parser_reset_message(impl);
  chttp_server_parser_reset_wire(impl);
  return SALTS_OK;
}

void chttp_server_parser_destroy(chttp_server_parser *parser) {
  chttp_server_parser_impl *impl;
  if (parser == NULL || parser->impl == NULL) return;
  impl = (chttp_server_parser_impl *)parser->impl;
  chttp_server_parser_close_body(impl, SALTS_ECANCELED);
  if (impl->body_storage != NULL) {
    if (impl->buffer_release != NULL)
      impl->buffer_release(impl->buffer_context, impl->body_storage,
                           impl->body_storage_capacity);
    else free(impl->body_storage);
  }
  free(impl->header_storage);
  free(impl->path_storage);
  free(impl->target_storage);
  free(impl->headers);
  free(impl);
  parser->impl = NULL;
}
