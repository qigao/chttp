#ifndef HTTP_COMMON_HTTP_H
#define HTTP_COMMON_HTTP_H

#include <cnet/cnet.h>
#include <salts/error_codes.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Reusable immutable HTTPS policy and connection-pool identity. */
typedef struct chttp_tls_profile {
  void *impl;
} chttp_tls_profile;

/** Wire protocol selected for one request. Zero preserves the HTTP/1.1 default. */
typedef enum chttp_protocol { CHTTP_HTTP_1_1 = 0, CHTTP_HTTP_2 = 1 } chttp_protocol;

typedef enum chttp_method {
  CHTTP_METHOD_GET = 1,
  CHTTP_METHOD_HEAD,
  CHTTP_METHOD_POST,
  CHTTP_METHOD_PUT,
  CHTTP_METHOD_DELETE,
  CHTTP_METHOD_PATCH,
  CHTTP_METHOD_OPTIONS,
  /** Observed by RFC 8441 WebSocket callbacks; not a generic routable method. */
  CHTTP_METHOD_CONNECT
} chttp_method;

/** Input strings are borrowed until submit returns; response strings are callback-scoped. */
typedef struct chttp_header {
  const char *name;
  const char *value;
} chttp_header;

/** Optional registered claims used to issue one HS256 JWT. Zero time values are omitted. */
typedef struct chttp_jwt_claims {
  const char *issuer;
  const char *subject;
  const char *audience;
  const char *jwt_id;
  int64_t issued_at;
  int64_t not_before;
  int64_t expires_at;
} chttp_jwt_claims;

/** Creates an owned compact HS256 JWT. The caller releases it with chttp_jwt_token_destroy(). */
int chttp_jwt_hs256_token_create(const chttp_jwt_claims *claims, const void *key, size_t key_size,
                                 char **out_token);

/** Releases a token returned by chttp_jwt_hs256_token_create(); NULL is accepted. */
void chttp_jwt_token_destroy(char *token);

/**
 * Pulls at most `capacity` bytes into callback-scoped storage. Return
 * `SALTS_OK` and set `out_size` to zero for EOF, or return a negative error.
 * The callback runs on the CHTTP owner thread and must not perform unbounded blocking or reenter
 * it.
 */
typedef int (*chttp_body_read_fn)(void *user, void *buffer, size_t capacity, size_t *out_size);

/**
 * Consumes one callback-scoped body view. A successful return transfers the
 * whole view; a negative return terminates the request without partial credit.
 * The callback runs on the CHTTP owner thread and must not perform unbounded blocking or reenter
 * it.
 */
typedef int (*chttp_body_write_fn)(void *user, const void *data, size_t size);

typedef struct chttp_body_source {
  chttp_body_read_fn read;
  void *user;
  size_t content_length;
  int content_length_known;
} chttp_body_source;

typedef struct chttp_body_sink {
  chttp_body_write_fn write;
  void *user;
} chttp_body_sink;

typedef void (*chttp_progress_fn)(void *user, size_t transferred, size_t total);

typedef enum chttp_websocket_state {
  CHTTP_WEBSOCKET_OPEN = 1,
  CHTTP_WEBSOCKET_CLOSING,
  CHTTP_WEBSOCKET_CLOSED,
  CHTTP_WEBSOCKET_FAILED
} chttp_websocket_state;

typedef enum chttp_websocket_message_type {
  CHTTP_WEBSOCKET_MESSAGE_NONE = 0,
  CHTTP_WEBSOCKET_MESSAGE_TEXT,
  CHTTP_WEBSOCKET_MESSAGE_BINARY
} chttp_websocket_message_type;

typedef enum chttp_websocket_event_kind {
  CHTTP_WEBSOCKET_EVENT_MESSAGE = 1,
  CHTTP_WEBSOCKET_EVENT_PING,
  CHTTP_WEBSOCKET_EVENT_PONG,
  CHTTP_WEBSOCKET_EVENT_CLOSE
} chttp_websocket_event_kind;

/** Server event data is borrowed only until the callback returns; client lifetime is documented
 * below. */
typedef struct chttp_websocket_event {
  chttp_websocket_event_kind kind;
  chttp_websocket_message_type message_type;
  const uint8_t *data;
  size_t size;
  uint16_t close_code;
} chttp_websocket_event;

/**
 * Builds a reusable verified TLS profile. Configuration is consumed before
 * return. ALPN must be absent or contain exactly one of `http/1.1` or `h2`.
 * A profile is protocol-specific and cannot be shared across H1 and H2 requests. The same public
 * wrapper must not be initialized/destroyed concurrently with submit.
 *
 * @param profile Zero-initialized reusable output profile.
 * @param config Explicit CNet TLS client policy.
 * @return CNet TLS setup errors, or `SALTS_ENOTSUP` for an unsupported ALPN list.
 */
int chttp_tls_profile_init(chttp_tls_profile *profile, const cnet_tls_client_config *config);

/**
 * Releases the public reference; admitted requests and idle slots remain
 * valid. Repeated destroy succeeds; a NULL wrapper returns `SALTS_EINVAL`.
 */
int chttp_tls_profile_destroy(chttp_tls_profile *profile);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_COMMON_HTTP_H */
