#include <chttp_web/web.h>

#include <openssl/crypto.h>
#include <platform.h>
#include <salts/error_codes.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHTTP_WEB_CSRF_SESSION_KEY "__chttp_web_csrf_v1"
#define CHTTP_WEB_FLASH_SESSION_KEY "__chttp_web_flash_v1"
#define CHTTP_WEB_FLASH_VERSION_PREFIX "1;"
#define CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES 2u

typedef struct chttp_web_flash_scan {
  size_t count;
  size_t bytes;
} chttp_web_flash_scan;

static chttp_web_status chttp_web_session_fail(
    chttp_web_error *error, chttp_web_status status, int native_status,
    size_t offset, const char *message) {
  if (error != NULL) {
    error->status = status;
    error->native_status = native_status;
    error->offset = offset;
    error->template_name[0] = '\0';
    if (message != NULL)
      (void)snprintf(error->message, sizeof(error->message), "%s", message);
    else
      error->message[0] = '\0';
  }
  return status;
}

static chttp_web_status chttp_web_session_status(
    chttp_web_error *error, int status, const char *message) {
  if (status == SALTS_OK)
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
  if (status == SALTS_ENOBUFS || status == SALTS_EMSGSIZE)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, status, 0u, message);
  return chttp_web_session_fail(
      error, CHTTP_WEB_SERVER, status, 0u, message);
}

static int chttp_web_csrf_token_valid(const char *token) {
  size_t i;
  if (token == NULL || strlen(token) != CHTTP_WEB_CSRF_TOKEN_BYTES) return 0;
  for (i = 0u; i < CHTTP_WEB_CSRF_TOKEN_BYTES; ++i) {
    const unsigned char ch = (unsigned char)token[i];
    if (!((ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
          (ch >= (unsigned char)'a' && ch <= (unsigned char)'f')))
      return 0;
  }
  return 1;
}

const char *chttp_web_csrf_token(const chttp_session *session) {
  const char *token;
  if (session == NULL) return NULL;
  token = chttp_session_get(session, CHTTP_WEB_CSRF_SESSION_KEY);
  return chttp_web_csrf_token_valid(token) ? token : NULL;
}

static chttp_web_status chttp_web_csrf_write(
    chttp_session *session, const char **out_token, chttp_web_error *error) {
  static const char hex[] = "0123456789abcdef";
  unsigned char random[CHTTP_WEB_CSRF_RANDOM_BYTES];
  char token[CHTTP_WEB_CSRF_TOKEN_BYTES + 1u];
  size_t i;
  int status;

  if (out_token != NULL) *out_token = NULL;
  if (session == NULL || out_token == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "CSRF session and output are required");

  status = salts_secure_random(random, sizeof(random));
  if (status != SALTS_OK) {
    OPENSSL_cleanse(random, sizeof(random));
    return chttp_web_session_status(
        error, status, "secure random generation failed");
  }
  for (i = 0u; i < sizeof(random); ++i) {
    token[i * 2u] = hex[random[i] >> 4u];
    token[i * 2u + 1u] = hex[random[i] & 0x0fu];
  }
  token[CHTTP_WEB_CSRF_TOKEN_BYTES] = '\0';
  OPENSSL_cleanse(random, sizeof(random));

  status = chttp_session_set(session, CHTTP_WEB_CSRF_SESSION_KEY, token);
  OPENSSL_cleanse(token, sizeof(token));
  if (status != SALTS_OK)
    return chttp_web_session_status(
        error, status, "CHTTP session rejected the CSRF token");

  *out_token = chttp_web_csrf_token(session);
  if (*out_token == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_SERVER, SALTS_EIO, 0u,
        "stored CSRF token is unavailable");
  return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

chttp_web_status chttp_web_csrf_ensure(
    chttp_session *session, const char **out_token, chttp_web_error *error) {
  const char *existing;
  if (out_token != NULL) *out_token = NULL;
  if (session == NULL || out_token == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "CSRF session and output are required");
  existing = chttp_web_csrf_token(session);
  if (existing != NULL) {
    *out_token = existing;
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
  }
  return chttp_web_csrf_write(session, out_token, error);
}

chttp_web_status chttp_web_csrf_rotate(
    chttp_session *session, const char **out_token, chttp_web_error *error) {
  return chttp_web_csrf_write(session, out_token, error);
}

chttp_web_status chttp_web_csrf_clear(
    chttp_session *session, chttp_web_error *error) {
  int status;
  if (session == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "CSRF session is required");
  status = chttp_session_remove(session, CHTTP_WEB_CSRF_SESSION_KEY);
  if (status == SALTS_ENOENT)
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
  return chttp_web_session_status(
      error, status, "unable to clear CSRF token");
}

static int chttp_web_csrf_method_unsafe(chttp_method method) {
  return method == CHTTP_METHOD_POST || method == CHTTP_METHOD_PUT ||
         method == CHTTP_METHOD_PATCH || method == CHTTP_METHOD_DELETE;
}

chttp_web_status chttp_web_csrf_validate_token(
    const chttp_server_request_view *request,
    const void *token,
    size_t token_size,
    chttp_web_error *error) {
  const char *expected;
  if (request == NULL || (token_size != 0u && token == NULL))
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "CSRF request/token arguments are invalid");
  if (!chttp_web_csrf_method_unsafe(request->method))
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);

  expected = chttp_web_csrf_token(request->session);
  if (expected == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CSRF, 0, 0u,
        "CSRF session token is missing");
  if (token == NULL || token_size != CHTTP_WEB_CSRF_TOKEN_BYTES)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CSRF, 0, 0u,
        "CSRF token is missing or malformed");
  if (CRYPTO_memcmp(expected, token, CHTTP_WEB_CSRF_TOKEN_BYTES) != 0)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CSRF, 0, 0u,
        "CSRF token does not match the current session");
  return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

chttp_web_status chttp_web_csrf_validate(
    const chttp_server_request_view *request, const chttp_web_form *form,
    chttp_web_error *error) {
  const char *header = NULL;
  const void *candidate = NULL;
  size_t candidate_size = 0u;

  if (request == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "CSRF request is required");

  if (chttp_web_request_is_htmx(request))
    header = chttp_server_request_header(request, CHTTP_WEB_CSRF_HEADER);
  if (header != NULL) {
    candidate = header;
    candidate_size = strlen(header);
  } else if (form != NULL) {
    const size_t count =
        chttp_web_form_count(form, CHTTP_WEB_CSRF_FORM_FIELD);
    const chttp_web_form_pair *pair;
    if (count != 1u && chttp_web_csrf_method_unsafe(request->method))
      return chttp_web_session_fail(
          error, CHTTP_WEB_CSRF, 0, 0u,
          "CSRF form token is missing or repeated");
    pair = count == 1u
        ? chttp_web_form_get(form, CHTTP_WEB_CSRF_FORM_FIELD, 0u)
        : NULL;
    if (pair != NULL) {
      candidate = pair->value.data;
      candidate_size = pair->value.size;
    }
  }

  return chttp_web_csrf_validate_token(
      request, candidate, candidate_size, error);
}

static int chttp_web_flash_config_valid(const chttp_web_flash_config *config) {
  return config != NULL && config->size >= sizeof(*config) &&
         config->max_messages != 0u &&
         config->max_messages <= CHTTP_WEB_FLASH_HARD_MAX_MESSAGES &&
         config->max_level_bytes != 0u &&
         config->max_text_bytes != 0u &&
         config->max_serialized_bytes >= CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES &&
         config->max_serialized_bytes <=
             CHTTP_WEB_FLASH_HARD_MAX_SERIALIZED_BYTES;
}

static int chttp_web_flash_parse_size(
    const char *encoded, size_t length, size_t *position, size_t *out) {
  size_t value = 0u;
  size_t start;
  if (encoded == NULL || position == NULL || out == NULL ||
      *position >= length)
    return 0;
  start = *position;
  while (*position < length && encoded[*position] != ':') {
    const unsigned char ch = (unsigned char)encoded[*position];
    size_t digit;
    if (ch < (unsigned char)'0' || ch > (unsigned char)'9') return 0;
    digit = (size_t)(ch - (unsigned char)'0');
    if (value > (SIZE_MAX - digit) / 10u) return 0;
    value = value * 10u + digit;
    ++(*position);
  }
  if (*position == start || *position >= length ||
      encoded[*position] != ':')
    return 0;
  ++(*position);
  *out = value;
  return 1;
}

static chttp_web_status chttp_web_flash_scan_queue(
    const char *encoded, const chttp_web_flash_config *config,
    const chttp_web_flash_buffer *buffer, chttp_web_flash_scan *out,
    chttp_web_error *error) {
  size_t length;
  size_t position;
  size_t count = 0u;
  size_t bytes = 0u;
  size_t byte_cursor = 0u;

  if (out != NULL) *out = (chttp_web_flash_scan){0};
  if (!chttp_web_flash_config_valid(config) || out == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "flash configuration is invalid");
  if (encoded == NULL)
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);

  length = strlen(encoded);
  if (length > config->max_serialized_bytes ||
      length < CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES ||
      memcmp(encoded, CHTTP_WEB_FLASH_VERSION_PREFIX,
             CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES) != 0)
    return chttp_web_session_fail(
        error, CHTTP_WEB_FLASH, 0, 0u,
        "flash session payload is invalid");
  position = CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES;

  while (position < length) {
    size_t level_size;
    size_t text_size;
    if (count >= config->max_messages ||
        !chttp_web_flash_parse_size(
            encoded, length, &position, &level_size) ||
        !chttp_web_flash_parse_size(
            encoded, length, &position, &text_size) ||
        level_size > config->max_level_bytes ||
        text_size > config->max_text_bytes ||
        level_size > length - position ||
        text_size > length - position - level_size)
      return chttp_web_session_fail(
          error, CHTTP_WEB_FLASH, 0, position,
          "flash session payload is malformed");
    if (level_size > SIZE_MAX - text_size ||
        bytes > SIZE_MAX - level_size - text_size)
      return chttp_web_session_fail(
          error, CHTTP_WEB_CAPACITY, 0, position,
          "flash decoded bytes overflow");

    if (buffer != NULL) {
      chttp_web_flash_message *message;
      if (buffer->size < sizeof(*buffer) ||
          buffer->message_storage == NULL ||
          buffer->byte_storage == NULL ||
          count >= buffer->message_capacity ||
          byte_cursor > buffer->byte_capacity ||
          level_size + text_size >
              buffer->byte_capacity - byte_cursor)
        return chttp_web_session_fail(
            error, CHTTP_WEB_CAPACITY, 0, position,
            "flash output storage is too small");
      message = &buffer->message_storage[count];
      message->level = (chttp_web_string_view){
          buffer->byte_storage + byte_cursor, level_size};
      if (level_size != 0u)
        memcpy(buffer->byte_storage + byte_cursor,
               encoded + position, level_size);
      byte_cursor += level_size;
      position += level_size;
      message->text = (chttp_web_string_view){
          buffer->byte_storage + byte_cursor, text_size};
      if (text_size != 0u)
        memcpy(buffer->byte_storage + byte_cursor,
               encoded + position, text_size);
      byte_cursor += text_size;
      position += text_size;
    } else {
      position += level_size + text_size;
    }
    bytes += level_size + text_size;
    ++count;
  }

  out->count = count;
  out->bytes = bytes;
  return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

chttp_web_status chttp_web_flash_push(
    chttp_session *session, const chttp_web_flash_config *config,
    const char *level, const char *text, chttp_web_error *error) {
  char next[CHTTP_WEB_FLASH_HARD_MAX_SERIALIZED_BYTES + 1u];
  char header[64];
  const char *existing;
  chttp_web_flash_scan scan = {0};
  size_t existing_size;
  size_t level_size;
  size_t text_size;
  int header_size;
  size_t total;
  int status;
  chttp_web_status web_status;

  if (session == NULL || !chttp_web_flash_config_valid(config) ||
      level == NULL || text == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "flash push arguments are invalid");

  level_size = strlen(level);
  text_size = strlen(text);
  if (level_size > config->max_level_bytes ||
      text_size > config->max_text_bytes)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash message exceeds configured bounds");

  existing = chttp_session_get(session, CHTTP_WEB_FLASH_SESSION_KEY);
  web_status =
      chttp_web_flash_scan_queue(existing, config, NULL, &scan, error);
  if (web_status != CHTTP_WEB_OK) return web_status;
  if (scan.count >= config->max_messages)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash message count exceeds configured bound");

  existing_size = existing != NULL
      ? strlen(existing)
      : CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES;
  header_size = snprintf(
      header, sizeof(header), "%zu:%zu:", level_size, text_size);
  if (header_size < 0 || (size_t)header_size >= sizeof(header))
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash length header exceeds internal bound");
  if (existing_size > SIZE_MAX - (size_t)header_size ||
      existing_size + (size_t)header_size > SIZE_MAX - level_size ||
      existing_size + (size_t)header_size + level_size >
          SIZE_MAX - text_size)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash serialized size overflows");
  total = existing_size + (size_t)header_size + level_size + text_size;
  if (total > config->max_serialized_bytes ||
      total > CHTTP_WEB_FLASH_HARD_MAX_SERIALIZED_BYTES)
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash queue exceeds configured serialized bound");

  if (existing != NULL)
    memcpy(next, existing, existing_size);
  else
    memcpy(next, CHTTP_WEB_FLASH_VERSION_PREFIX,
           CHTTP_WEB_FLASH_VERSION_PREFIX_BYTES);
  memcpy(next + existing_size, header, (size_t)header_size);
  memcpy(next + existing_size + (size_t)header_size, level, level_size);
  memcpy(next + existing_size + (size_t)header_size + level_size,
         text, text_size);
  next[total] = '\0';

  status = chttp_session_set(session, CHTTP_WEB_FLASH_SESSION_KEY, next);
  OPENSSL_cleanse(next, sizeof(next));
  return chttp_web_session_status(
      error, status, "CHTTP session rejected flash state");
}

chttp_web_status chttp_web_flash_consume(
    chttp_session *session, const chttp_web_flash_config *config,
    const chttp_web_flash_buffer *buffer, size_t *out_count,
    chttp_web_error *error) {
  const char *encoded;
  chttp_web_flash_scan scan = {0};
  chttp_web_status status;
  int session_status;

  if (out_count != NULL) *out_count = 0u;
  if (session == NULL || !chttp_web_flash_config_valid(config) ||
      buffer == NULL || buffer->size < sizeof(*buffer) ||
      out_count == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "flash consume arguments are invalid");

  encoded = chttp_session_get(session, CHTTP_WEB_FLASH_SESSION_KEY);
  if (encoded == NULL)
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);

  status = chttp_web_flash_scan_queue(
      encoded, config, NULL, &scan, error);
  if (status != CHTTP_WEB_OK) return status;
  if (scan.count > buffer->message_capacity ||
      scan.bytes > buffer->byte_capacity ||
      (scan.count != 0u && buffer->message_storage == NULL) ||
      (scan.bytes != 0u && buffer->byte_storage == NULL))
    return chttp_web_session_fail(
        error, CHTTP_WEB_CAPACITY, 0, 0u,
        "flash output storage is too small");

  status = chttp_web_flash_scan_queue(
      encoded, config, buffer, &scan, error);
  if (status != CHTTP_WEB_OK) return status;

  session_status =
      chttp_session_remove(session, CHTTP_WEB_FLASH_SESSION_KEY);
  if (session_status != SALTS_OK) {
    *out_count = 0u;
    return chttp_web_session_status(
        error, session_status, "unable to consume flash state");
  }
  *out_count = scan.count;
  return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
}

chttp_web_status chttp_web_flash_clear(
    chttp_session *session, chttp_web_error *error) {
  int status;
  if (session == NULL)
    return chttp_web_session_fail(
        error, CHTTP_WEB_INVALID_ARGUMENT, 0, 0u,
        "flash session is required");
  status = chttp_session_remove(session, CHTTP_WEB_FLASH_SESSION_KEY);
  if (status == SALTS_ENOENT)
    return chttp_web_session_fail(error, CHTTP_WEB_OK, 0, 0u, NULL);
  return chttp_web_session_status(
      error, status, "unable to clear flash state");
}
