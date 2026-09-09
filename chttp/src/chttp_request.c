#include "chttp_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHTTP_GENERATED_HEADER_COUNT = 3 };

static bool chttp_size_add(size_t left, size_t right, size_t *out) {
  if (out == NULL || left > SIZE_MAX - right) return false;
  *out = left + right;
  return true;
}

static int chttp_bounded_length(const char *text, size_t limit, size_t *out_length) {
  size_t index;
  if (text == NULL || out_length == NULL) return SALTS_EINVAL;
  for (index = 0u; index <= limit; ++index) {
    if (text[index] == '\0') {
      *out_length = index;
      return SALTS_OK;
    }
  }
  return SALTS_EMSGSIZE;
}

static unsigned char chttp_ascii_lower(unsigned char value) {
  return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static bool chttp_ascii_equal(const char *left, const char *right) {
  size_t index = 0u;
  if (left == NULL || right == NULL) return false;
  while (left[index] != '\0' && right[index] != '\0') {
    if (chttp_ascii_lower((unsigned char)left[index]) !=
        chttp_ascii_lower((unsigned char)right[index]))
      return false;
    ++index;
  }
  return left[index] == '\0' && right[index] == '\0';
}

static bool chttp_header_name_byte(unsigned char value) {
  if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9'))
    return true;
  return value == '!' || value == '#' || value == '$' || value == '%' || value == '&' ||
         value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' ||
         value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

static int chttp_header_valid(const chttp_header *header, size_t limit, size_t *out_name_size,
                              size_t *out_value_size) {
  size_t name_size = 0u;
  size_t value_size = 0u;
  size_t index;
  int status;
  if (header == NULL || out_name_size == NULL || out_value_size == NULL) return SALTS_EINVAL;
  status = chttp_bounded_length(header->name, limit, &name_size);
  if (status != SALTS_OK) return status;
  status = chttp_bounded_length(header->value, limit, &value_size);
  if (status != SALTS_OK) return status;
  if (name_size == 0u) return SALTS_EINVAL;
  for (index = 0u; index < name_size; ++index)
    if (!chttp_header_name_byte((unsigned char)header->name[index])) return SALTS_EINVAL;
  for (index = 0u; index < value_size; ++index) {
    const unsigned char value = (unsigned char)header->value[index];
    if ((value < 0x20u && value != '\t') || value == 0x7fu) return SALTS_EINVAL;
  }
  if (chttp_ascii_equal(header->name, "host") ||
      chttp_ascii_equal(header->name, "content-length") ||
      chttp_ascii_equal(header->name, "transfer-encoding") ||
      chttp_ascii_equal(header->name, "connection"))
    return SALTS_EINVAL;
  *out_name_size = name_size;
  *out_value_size = value_size;
  return SALTS_OK;
}

static int chttp_authority_valid(const char *authority, size_t limit, size_t *out_size) {
  size_t size = 0u;
  size_t index;
  int status = chttp_bounded_length(authority, limit, &size);
  if (status != SALTS_OK) return status;
  if (size == 0u) return SALTS_EINVAL;
  for (index = 0u; index < size; ++index) {
    const unsigned char value = (unsigned char)authority[index];
    if (value <= 0x20u || value >= 0x7fu || value == '/' || value == '?' || value == '#' ||
        value == '@')
      return SALTS_EINVAL;
  }
  *out_size = size;
  return SALTS_OK;
}

static int chttp_target_valid(const char *target, size_t limit, chttp_method method,
                              size_t *out_size) {
  size_t size = 0u;
  size_t index;
  int status = chttp_bounded_length(target, limit, &size);
  if (status != SALTS_OK) return status;
  if (size == 0u || (target[0] != '/' && !(method == CHTTP_METHOD_OPTIONS && target[0] == '*')))
    return SALTS_EINVAL;
  if (target[0] == '*' && size != 1u) return SALTS_EINVAL;
  for (index = 0u; index < size; ++index) {
    const unsigned char value = (unsigned char)target[index];
    if (value <= 0x20u || value >= 0x7fu || value == '#') return SALTS_EINVAL;
  }
  *out_size = size;
  return SALTS_OK;
}

static const char *chttp_method_name(chttp_method method) {
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

static bool chttp_add_header_size(size_t name_size, size_t value_size, size_t *header_bytes) {
  size_t line_size;
  if (!chttp_size_add(name_size, value_size, &line_size) ||
      !chttp_size_add(line_size, 4u, &line_size))
    return false;
  return chttp_size_add(*header_bytes, line_size, header_bytes);
}

static unsigned char *chttp_copy(unsigned char *cursor, const void *data, size_t size) {
  if (size != 0u) memcpy(cursor, data, size);
  return cursor + size;
}

int chttp_request_build(const chttp_request_options *options, const chttp_limits *limits,
                        unsigned char **out_data, size_t *out_size) {
  static const char http_suffix[] = " HTTP/1.1\r\n";
  static const char host_prefix[] = "Host: ";
  static const char length_prefix[] = "Content-Length: ";
  static const char chunked_header[] = "Transfer-Encoding: chunked\r\n";
  static const char keep_alive_header[] = "Connection: keep-alive\r\n";
  const char *method_name;
  size_t method_size;
  size_t target_size = 0u;
  size_t authority_size = 0u;
  size_t header_bytes = 0u;
  size_t request_line_bytes;
  size_t total_size;
  size_t serialized_body_size;
  size_t declared_body_size;
  size_t index;
  char body_size_text[3u * sizeof(size_t) + 1u];
  int body_size_chars;
  unsigned char *data;
  unsigned char *cursor;
  int status;

  if (out_data == NULL || out_size == NULL) return SALTS_EINVAL;
  *out_data = NULL;
  *out_size = 0u;
  if (options == NULL || limits == NULL || options->connection_uri == NULL ||
      options->on_complete == NULL || (options->header_count != 0u && options->headers == NULL) ||
      (options->body_size != 0u && options->body == NULL) ||
      (options->body_source != NULL &&
       (options->body != NULL || options->body_size != 0u || options->body_source->read == NULL ||
        (options->body_source->content_length_known != 0 &&
         options->body_source->content_length_known != 1))))
    return SALTS_EINVAL;
  declared_body_size = options->body_source != NULL && options->body_source->content_length_known
                           ? options->body_source->content_length
                           : options->body_size;
  serialized_body_size = options->body_source == NULL ? options->body_size : 0u;
  if (declared_body_size > limits->max_request_body_bytes) return SALTS_EMSGSIZE;
  if (options->header_count > SIZE_MAX - CHTTP_GENERATED_HEADER_COUNT ||
      options->header_count + CHTTP_GENERATED_HEADER_COUNT > limits->max_header_count)
    return SALTS_EMSGSIZE;

  method_name = chttp_method_name(options->method);
  if (method_name == NULL) return SALTS_EINVAL;
  method_size = strlen(method_name);
  status = chttp_target_valid(options->target, limits->max_start_line_bytes, options->method,
                              &target_size);
  if (status != SALTS_OK) return status;
  status = chttp_authority_valid(options->authority, limits->max_header_bytes, &authority_size);
  if (status != SALTS_OK) return status;

  if (!chttp_size_add(method_size, 1u, &request_line_bytes) ||
      !chttp_size_add(request_line_bytes, target_size, &request_line_bytes) ||
      !chttp_size_add(request_line_bytes, sizeof(http_suffix) - 1u, &request_line_bytes))
    return SALTS_EMSGSIZE;
  if (request_line_bytes > limits->max_start_line_bytes) return SALTS_EMSGSIZE;

  body_size_chars = snprintf(body_size_text, sizeof(body_size_text), "%zu", declared_body_size);
  if (body_size_chars <= 0 || (size_t)body_size_chars >= sizeof(body_size_text))
    return SALTS_ERANGE;
  if (!chttp_add_header_size(sizeof("Host") - 1u, authority_size, &header_bytes) ||
      !chttp_add_header_size(sizeof("Connection") - 1u, sizeof("keep-alive") - 1u, &header_bytes))
    return SALTS_EMSGSIZE;
  if (options->body_source != NULL && !options->body_source->content_length_known) {
    if (!chttp_add_header_size(sizeof("Transfer-Encoding") - 1u, sizeof("chunked") - 1u,
                               &header_bytes))
      return SALTS_EMSGSIZE;
  } else if (!chttp_add_header_size(sizeof("Content-Length") - 1u, (size_t)body_size_chars,
                                    &header_bytes)) {
    return SALTS_EMSGSIZE;
  }

  for (index = 0u; index < options->header_count; ++index) {
    size_t name_size = 0u;
    size_t value_size = 0u;
    status = chttp_header_valid(&options->headers[index], limits->max_header_bytes, &name_size,
                                &value_size);
    if (status != SALTS_OK) return status;
    if (!chttp_add_header_size(name_size, value_size, &header_bytes)) return SALTS_EMSGSIZE;
  }
  if (header_bytes > limits->max_header_bytes) return SALTS_EMSGSIZE;
  if (!chttp_size_add(request_line_bytes, header_bytes, &total_size) ||
      !chttp_size_add(total_size, 2u, &total_size) ||
      !chttp_size_add(total_size, serialized_body_size, &total_size) ||
      total_size > limits->max_request_bytes)
    return SALTS_EMSGSIZE;

  data = (unsigned char *)malloc(total_size);
  if (data == NULL) return SALTS_ENOMEM;
  cursor = data;
  cursor = chttp_copy(cursor, method_name, method_size);
  *cursor++ = ' ';
  cursor = chttp_copy(cursor, options->target, target_size);
  cursor = chttp_copy(cursor, http_suffix, sizeof(http_suffix) - 1u);
  cursor = chttp_copy(cursor, host_prefix, sizeof(host_prefix) - 1u);
  cursor = chttp_copy(cursor, options->authority, authority_size);
  cursor = chttp_copy(cursor, "\r\n", 2u);
  if (options->body_source != NULL && !options->body_source->content_length_known) {
    cursor = chttp_copy(cursor, chunked_header, sizeof(chunked_header) - 1u);
  } else {
    cursor = chttp_copy(cursor, length_prefix, sizeof(length_prefix) - 1u);
    cursor = chttp_copy(cursor, body_size_text, (size_t)body_size_chars);
    cursor = chttp_copy(cursor, "\r\n", 2u);
  }
  cursor = chttp_copy(cursor, keep_alive_header, sizeof(keep_alive_header) - 1u);
  for (index = 0u; index < options->header_count; ++index) {
    const size_t name_size = strlen(options->headers[index].name);
    const size_t value_size = strlen(options->headers[index].value);
    cursor = chttp_copy(cursor, options->headers[index].name, name_size);
    cursor = chttp_copy(cursor, ": ", 2u);
    cursor = chttp_copy(cursor, options->headers[index].value, value_size);
    cursor = chttp_copy(cursor, "\r\n", 2u);
  }
  cursor = chttp_copy(cursor, "\r\n", 2u);
  (void)chttp_copy(cursor, options->body, serialized_body_size);
  *out_data = data;
  *out_size = total_size;
  return SALTS_OK;
}

const char *chttp_response_view_header(const chttp_response_view *response, const char *name) {
  size_t index;
  if (response == NULL || name == NULL) return NULL;
  for (index = 0u; index < response->header_count; ++index)
    if (chttp_ascii_equal(response->headers[index].name, name))
      return response->headers[index].value;
  return NULL;
}
