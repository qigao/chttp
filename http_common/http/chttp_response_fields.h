#ifndef CHTTP_RESPONSE_FIELDS_H
#define CHTTP_RESPONSE_FIELDS_H
/* Shared pure response-field policy. No builder, storage or thread ownership. */
#include <core/error_codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool chttp_response_ascii_equal(const char *left, const char *right) {
  if (!left || !right) return false;
  unsigned char a, b;
  do {
    a = (unsigned char)*left++; b = (unsigned char)*right++;
    if (a >= 'A' && a <= 'Z') a += 32u;
    if (b >= 'A' && b <= 'Z') b += 32u;
    if (a != b) return false;
  } while (a);
  return true;
}
static inline bool chttp_response_token(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (!cursor || !*cursor) return false;
  for (; *cursor; ++cursor) {
    const unsigned char ch = *cursor;
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) continue;
    if (!strchr("!#$%&'*+-.^_`|~", (int)ch)) return false;
  }
  return true;
}
static inline bool chttp_response_value(const char *value) {
  const unsigned char *cursor = (const unsigned char *)value;
  if (!cursor) return false;
  for (; *cursor; ++cursor) if ((*cursor < 32u && *cursor != '\t') || *cursor == 127u) return false;
  return true;
}
typedef struct chttp_response_field_size { size_t name, value, wire; } chttp_response_field_size;
static inline int chttp_response_field_measure(const char *name, const char *value, bool websocket_protocol,
    chttp_response_field_size *out) {
  if (!chttp_response_token(name) || !chttp_response_value(value)) return SALTS_EINVAL;
  if (chttp_response_ascii_equal(name, "Content-Length") || chttp_response_ascii_equal(name, "Connection") ||
      chttp_response_ascii_equal(name, "Transfer-Encoding") || chttp_response_ascii_equal(name, "Upgrade") ||
      chttp_response_ascii_equal(name, "Sec-WebSocket-Accept") || chttp_response_ascii_equal(name, "Sec-WebSocket-Extensions") ||
      (!websocket_protocol && chttp_response_ascii_equal(name, "Sec-WebSocket-Protocol"))) return SALTS_EPERM;
  const size_t name_size = strlen(name), value_size = strlen(value);
  if (value_size > SIZE_MAX - 4u || name_size > SIZE_MAX - value_size - 4u) return SALTS_ERANGE;
  const chttp_response_field_size size = {name_size, value_size, name_size + value_size + 4u};
  *out = size;
  return SALTS_OK;
}
#endif
