#include "chttp_websocket_handshake.h"

#include <base64_utils.h>

#include <llhttp.h>
#include <openssl/evp.h>
#include <salts/random.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHTTP_WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum {
  CHTTP_WEBSOCKET_NONCE_BYTES = 16,
  CHTTP_WEBSOCKET_SHA1_BYTES = 20,
  CHTTP_WEBSOCKET_ACCEPT_SOURCE_BYTES = CHTTP_WEBSOCKET_KEY_BYTES + sizeof(CHTTP_WEBSOCKET_GUID) - 1
};

static unsigned char chttp_websocket_ascii_lower(unsigned char value) {
  return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
             ? (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'))
             : value;
}

static bool chttp_websocket_ascii_equal_n(const char *left, size_t left_size, const char *right) {
  size_t index;
  if (left == NULL || right == NULL || strlen(right) != left_size) return false;
  for (index = 0u; index < left_size; ++index)
    if (chttp_websocket_ascii_lower((unsigned char)left[index]) !=
        chttp_websocket_ascii_lower((unsigned char)right[index]))
      return false;
  return true;
}

static bool chttp_websocket_header_name(const chttp_header *header, const char *name) {
  return header != NULL && header->name != NULL &&
         chttp_websocket_ascii_equal_n(header->name, strlen(header->name), name);
}

static const char *chttp_websocket_trim_ows(const char *value, size_t *out_size) {
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

static bool chttp_websocket_header_has_token(const char *value, const char *wanted) {
  const char *cursor = value;
  if (value == NULL || wanted == NULL) return false;
  while (*cursor != '\0') {
    const char *end = strchr(cursor, ',');
    const char *token_end;
    if (end == NULL) end = cursor + strlen(cursor);
    while (cursor != end && (*cursor == ' ' || *cursor == '\t'))
      ++cursor;
    token_end = end;
    while (token_end != cursor && (token_end[-1] == ' ' || token_end[-1] == '\t'))
      --token_end;
    if (chttp_websocket_ascii_equal_n(cursor, (size_t)(token_end - cursor), wanted)) return true;
    cursor = *end == ',' ? end + 1 : end;
  }
  return false;
}

static bool chttp_websocket_content_length_zero(const char *value) {
  size_t size = 0u;
  const char *trimmed = chttp_websocket_trim_ows(value, &size);
  size_t index;
  if (trimmed == NULL || size == 0u) return false;
  for (index = 0u; index < size; ++index)
    if (trimmed[index] != '0') return false;
  return true;
}

int chttp_websocket_accept_compute(const char *key, char *output, size_t output_capacity) {
  unsigned char source[CHTTP_WEBSOCKET_ACCEPT_SOURCE_BYTES];
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0u;
  if (key == NULL || output == NULL || strlen(key) != CHTTP_WEBSOCKET_KEY_BYTES ||
      output_capacity < CHTTP_WEBSOCKET_ACCEPT_CAPACITY)
    return SALTS_EINVAL;
  memcpy(source, key, CHTTP_WEBSOCKET_KEY_BYTES);
  memcpy(source + CHTTP_WEBSOCKET_KEY_BYTES, CHTTP_WEBSOCKET_GUID,
         sizeof(CHTTP_WEBSOCKET_GUID) - 1u);
  if (EVP_Digest(source, sizeof(source), digest, &digest_size, EVP_sha1(), NULL) != 1 ||
      digest_size != CHTTP_WEBSOCKET_SHA1_BYTES)
    return SALTS_EIO;
  if (tn_base64_encode_buf_ex(digest, digest_size, output, output_capacity) != TN_BASE64_OK)
    return SALTS_EMSGSIZE;
  return strlen(output) == CHTTP_WEBSOCKET_ACCEPT_BYTES ? SALTS_OK : SALTS_EPROTO;
}

int chttp_websocket_client_key_generate(char *output, size_t output_capacity) {
  unsigned char nonce[CHTTP_WEBSOCKET_NONCE_BYTES];
  int status;
  if (output == NULL || output_capacity < CHTTP_WEBSOCKET_KEY_CAPACITY) return SALTS_EINVAL;
  status = salts_platform_secure_random(nonce, sizeof(nonce));
  if (status != SALTS_OK) return status;
  return tn_base64_encode_buf_ex(nonce, sizeof(nonce), output, output_capacity) == TN_BASE64_OK
             ? SALTS_OK
             : SALTS_EMSGSIZE;
}

typedef struct chttp_websocket_client_handshake_parser {
  llhttp_t parser;
  llhttp_settings_t settings;
  char *field;
  char *value;
  const char *expected_accept;
  const char *expected_subprotocol;
  size_t capacity;
  size_t field_size;
  size_t value_size;
  size_t accept_count;
  size_t subprotocol_count;
  unsigned int http_status;
  bool upgrade;
  bool connection_upgrade;
  bool accept_matches;
  bool subprotocol_matches;
  bool headers_complete;
  bool protocol_version;
  bool invalid_framing;
  bool unsupported_negotiation;
  bool overflow;
} chttp_websocket_client_handshake_parser;

static int chttp_websocket_client_header_field(llhttp_t *parser, const char *at, size_t length) {
  chttp_websocket_client_handshake_parser *context =
      (chttp_websocket_client_handshake_parser *)parser->data;
  if (context == NULL || (length != 0u && at == NULL) || context->field_size > context->capacity ||
      length > context->capacity - context->field_size) {
    if (context != NULL) context->overflow = true;
    return HPE_USER;
  }
  if (length != 0u) memcpy(context->field + context->field_size, at, length);
  context->field_size += length;
  return 0;
}

static int chttp_websocket_client_header_field_complete(llhttp_t *parser) {
  chttp_websocket_client_handshake_parser *context =
      (chttp_websocket_client_handshake_parser *)parser->data;
  if (context == NULL || context->field_size >= context->capacity) return HPE_USER;
  context->field[context->field_size] = '\0';
  return 0;
}

static int chttp_websocket_client_header_value(llhttp_t *parser, const char *at, size_t length) {
  chttp_websocket_client_handshake_parser *context =
      (chttp_websocket_client_handshake_parser *)parser->data;
  if (context == NULL || (length != 0u && at == NULL) || context->value_size > context->capacity ||
      length > context->capacity - context->value_size) {
    if (context != NULL) context->overflow = true;
    return HPE_USER;
  }
  if (length != 0u) memcpy(context->value + context->value_size, at, length);
  context->value_size += length;
  return 0;
}

static int chttp_websocket_client_header_value_complete(llhttp_t *parser) {
  chttp_websocket_client_handshake_parser *context =
      (chttp_websocket_client_handshake_parser *)parser->data;
  if (context == NULL || context->value_size >= context->capacity) return HPE_USER;
  context->value[context->value_size] = '\0';
  if (chttp_websocket_ascii_equal_n(context->field, context->field_size, "Upgrade"))
    context->upgrade =
        context->upgrade || chttp_websocket_header_has_token(context->value, "websocket");
  else if (chttp_websocket_ascii_equal_n(context->field, context->field_size, "Connection"))
    context->connection_upgrade =
        context->connection_upgrade || chttp_websocket_header_has_token(context->value, "upgrade");
  else if (chttp_websocket_ascii_equal_n(context->field, context->field_size,
                                         "Sec-WebSocket-Accept")) {
    size_t size = 0u;
    const char *trimmed = chttp_websocket_trim_ows(context->value, &size);
    ++context->accept_count;
    context->accept_matches =
        trimmed != NULL && size == CHTTP_WEBSOCKET_ACCEPT_BYTES &&
        memcmp(trimmed, context->expected_accept, CHTTP_WEBSOCKET_ACCEPT_BYTES) == 0;
  } else if (chttp_websocket_ascii_equal_n(context->field, context->field_size, "Content-Length") ||
             chttp_websocket_ascii_equal_n(context->field, context->field_size,
                                           "Transfer-Encoding"))
    context->invalid_framing = true;
  else if (chttp_websocket_ascii_equal_n(context->field, context->field_size,
                                         "Sec-WebSocket-Protocol")) {
    size_t size = 0u;
    const char *trimmed = chttp_websocket_trim_ows(context->value, &size);
    ++context->subprotocol_count;
    context->subprotocol_matches =
        context->expected_subprotocol != NULL && trimmed != NULL &&
        size == strlen(context->expected_subprotocol) &&
        memcmp(trimmed, context->expected_subprotocol, size) == 0;
  } else if (chttp_websocket_ascii_equal_n(context->field, context->field_size,
                                            "Sec-WebSocket-Extensions"))
    context->unsupported_negotiation = true;
  context->field_size = 0u;
  context->value_size = 0u;
  return 0;
}

static int chttp_websocket_client_headers_complete(llhttp_t *parser) {
  chttp_websocket_client_handshake_parser *context =
      (chttp_websocket_client_handshake_parser *)parser->data;
  if (context == NULL) return HPE_USER;
  context->http_status = (unsigned int)llhttp_get_status_code(parser);
  context->protocol_version =
      llhttp_get_http_major(parser) == 1u && llhttp_get_http_minor(parser) == 1u;
  context->headers_complete = true;
  return 2;
}

int chttp_websocket_client_handshake_validate(const void *data, size_t size,
                                              const char *expected_accept,
                                              const char *expected_subprotocol,
                                              unsigned int *out_http_status) {
  chttp_websocket_client_handshake_parser context = {0};
  llhttp_errno_t parse_status;
  int status = SALTS_EPROTO;
  if (out_http_status != NULL) *out_http_status = 0u;
  if (data == NULL || size == 0u || expected_accept == NULL ||
      strlen(expected_accept) != CHTTP_WEBSOCKET_ACCEPT_BYTES || out_http_status == NULL ||
      size == SIZE_MAX)
    return SALTS_EINVAL;
  context.field = (char *)malloc(size + 1u);
  context.value = (char *)malloc(size + 1u);
  if (context.field == NULL || context.value == NULL) {
    status = SALTS_ENOMEM;
    goto cleanup;
  }
  context.expected_accept = expected_accept;
  context.expected_subprotocol = expected_subprotocol;
  context.capacity = size + 1u;
  llhttp_settings_init(&context.settings);
  context.settings.on_header_field = chttp_websocket_client_header_field;
  context.settings.on_header_field_complete = chttp_websocket_client_header_field_complete;
  context.settings.on_header_value = chttp_websocket_client_header_value;
  context.settings.on_header_value_complete = chttp_websocket_client_header_value_complete;
  context.settings.on_headers_complete = chttp_websocket_client_headers_complete;
  llhttp_init(&context.parser, HTTP_RESPONSE, &context.settings);
  context.parser.data = &context;
  parse_status = llhttp_execute(&context.parser, (const char *)data, size);
  *out_http_status = context.http_status;
  if ((parse_status == HPE_PAUSED_UPGRADE || parse_status == HPE_OK) && context.headers_complete &&
      context.protocol_version && context.http_status == 101u && context.upgrade &&
      context.connection_upgrade && context.accept_count == 1u && context.accept_matches &&
      ((expected_subprotocol == NULL && context.subprotocol_count == 0u) ||
       (expected_subprotocol != NULL && context.subprotocol_count == 1u &&
        context.subprotocol_matches)) &&
      !context.invalid_framing && !context.unsupported_negotiation)
    status = SALTS_OK;
  else if (context.overflow) status = SALTS_EMSGSIZE;

cleanup:
  free(context.value);
  free(context.field);
  return status;
}

static int chttp_websocket_key_validate(const char *value, char *key) {
  tn_base64_bytes_result_t decoded;
  char canonical[CHTTP_WEBSOCKET_KEY_CAPACITY];
  size_t size = 0u;
  const char *trimmed = chttp_websocket_trim_ows(value, &size);
  int status = SALTS_EPROTO;
  if (trimmed == NULL || size != CHTTP_WEBSOCKET_KEY_BYTES) return SALTS_EPROTO;
  memcpy(key, trimmed, size);
  key[size] = '\0';
  decoded = tn_base64_decode_ex(key);
  if (!decoded.ok) return decoded.error == TN_BASE64_ERR_NO_MEMORY ? SALTS_ENOMEM : SALTS_EPROTO;
  if (decoded.value.len == CHTTP_WEBSOCKET_NONCE_BYTES &&
      tn_base64_encode_buf_ex(decoded.value.data, decoded.value.len, canonical,
                              sizeof(canonical)) == TN_BASE64_OK &&
      memcmp(canonical, key, sizeof(canonical)) == 0)
    status = SALTS_OK;
  free(decoded.value.data);
  return status;
}

int chttp_websocket_server_handshake_validate(const chttp_server_request_view *request,
                                              char *accept, size_t accept_capacity,
                                              unsigned int *out_http_status) {
  char key[CHTTP_WEBSOCKET_KEY_CAPACITY];
  const char *key_value = NULL;
  const char *version_value = NULL;
  bool upgrade = false;
  bool connection_upgrade = false;
  size_t host_count = 0u;
  size_t key_count = 0u;
  size_t version_count = 0u;
  size_t index;
  int status;
  if (out_http_status != NULL) *out_http_status = 0u;
  if (request == NULL || accept == NULL || out_http_status == NULL ||
      accept_capacity < CHTTP_WEBSOCKET_ACCEPT_CAPACITY ||
      (request->header_count != 0u && request->headers == NULL))
    return SALTS_EINVAL;
  if (request->method != CHTTP_METHOD_GET || request->http_major != 1u ||
      request->http_minor != 1u || request->body_size != 0u || request->body_streamed != 0) {
    *out_http_status = 400u;
    return SALTS_EPROTO;
  }
  for (index = 0u; index < request->header_count; ++index) {
    const chttp_header *header = &request->headers[index];
    if (header->name == NULL || header->value == NULL) {
      *out_http_status = 400u;
      return SALTS_EPROTO;
    }
    if (chttp_websocket_header_name(header, "Host")) ++host_count;
    else if (chttp_websocket_header_name(header, "Upgrade"))
      upgrade = upgrade || chttp_websocket_header_has_token(header->value, "websocket");
    else if (chttp_websocket_header_name(header, "Connection"))
      connection_upgrade =
          connection_upgrade || chttp_websocket_header_has_token(header->value, "upgrade");
    else if (chttp_websocket_header_name(header, "Sec-WebSocket-Key")) {
      ++key_count;
      key_value = header->value;
    } else if (chttp_websocket_header_name(header, "Sec-WebSocket-Version")) {
      ++version_count;
      version_value = header->value;
    } else if (chttp_websocket_header_name(header, "Transfer-Encoding") ||
               (chttp_websocket_header_name(header, "Content-Length") &&
                !chttp_websocket_content_length_zero(header->value))) {
      *out_http_status = 400u;
      return SALTS_EPROTO;
    }
  }
  if (host_count != 1u || !upgrade || !connection_upgrade || key_count != 1u ||
      version_count != 1u) {
    *out_http_status = 400u;
    return SALTS_EPROTO;
  }
  {
    size_t version_size = 0u;
    const char *version = chttp_websocket_trim_ows(version_value, &version_size);
    if (version == NULL || version_size != 2u || memcmp(version, "13", 2u) != 0) {
      *out_http_status = 426u;
      return SALTS_EPROTONOSUPPORT;
    }
  }
  status = chttp_websocket_key_validate(key_value, key);
  if (status != SALTS_OK) {
    *out_http_status = status == SALTS_ENOMEM ? 500u : 400u;
    return status;
  }
  status = chttp_websocket_accept_compute(key, accept, accept_capacity);
  if (status != SALTS_OK) *out_http_status = status == SALTS_EMSGSIZE ? 500u : 400u;
  return status;
}
