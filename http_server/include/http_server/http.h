#ifndef HTTP_SERVER_HTTP_H
#define HTTP_SERVER_HTTP_H

#include <http_common/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Background HTTP/1.1 and optional HTTP/2 server owner; callers never drive a poller. */
typedef struct chttp_server {
  void *impl;
} chttp_server;

/**
 * One generation-checked deferred HTTP/1.1 or HTTP/2 response. The handle is completed
 * exactly once by `chttp_server_deferred_reply()` or
 * `chttp_server_deferred_cancel()` and must not outlive the server's successful
 * stop.
 */
typedef struct chttp_server_deferred {
  void *impl;
  uint32_t generation;
  uint32_t reserved;
} chttp_server_deferred;

#define CHTTP_SERVER_DEFERRED_INIT {NULL, 0u, 0u}

/** Handler-scoped server-side session. */
typedef struct chttp_session {
  void *impl;
} chttp_session;

/** Handler-scoped continuation for one middleware invocation. */
typedef struct chttp_server_next {
  void *impl;
} chttp_server_next;

/** Callback-scoped server WebSocket peer. The wrapper becomes invalid when its callback returns. */
typedef struct chttp_websocket {
  void *impl;
} chttp_websocket;

/**
 * Generation-checked server WebSocket session captured from a callback-scoped
 * peer. The value does not keep the connection alive and must not outlive its
 * server.
 */
typedef struct chttp_server_websocket_session {
  void *impl;
  uint32_t connection_slot;
  uint32_t connection_generation;
  int32_t stream_id;
} chttp_server_websocket_session;

/**
 * Verified JWT claims borrowed from the active server callback. Time pointers
 * are NULL when the corresponding registered claim was not present.
 */
typedef struct chttp_jwt_claims_view {
  const char *issuer;
  const char *subject;
  const char *jwt_id;
  const char *const *audiences;
  size_t audience_count;
  const int64_t *issued_at;
  const int64_t *not_before;
  const int64_t *expires_at;
} chttp_jwt_claims_view;

/**
 * Configuration copied by chttp_jwt_bearer_validator_init(). Issuer and
 * audience checks are optional when their corresponding pointers are NULL.
 */
typedef struct chttp_jwt_bearer_validator_options {
  size_t size;
  const void *key;
  size_t key_size;
  int64_t clock_skew_seconds;
  const char *expected_issuer;
  const char *expected_audience;
  int allow_missing_exp;
} chttp_jwt_bearer_validator_options;

/** Owns the key and expected claim values used by Bearer admission. */
typedef struct chttp_jwt_bearer_validator {
  void *impl;
} chttp_jwt_bearer_validator;

typedef struct chttp_server_param {
  const char *name;
  const char *value;
} chttp_server_param;

/**
 * Borrowed request view. Every pointer becomes invalid when the route handler
 * returns; handlers must copy data that outlives the callback.
 */
typedef struct chttp_server_request_view {
  unsigned int http_major;
  unsigned int http_minor;
  chttp_method method;
  /** Origin-form (or OPTIONS "*"); absolute-form is normalized before dispatch. */
  const char *target;
  const char *path;
  /** Absolute-form requests expose the URI authority as Host. */
  const chttp_header *headers;
  size_t header_count;
  const chttp_server_param *params;
  size_t param_count;
  const void *body;
  size_t body_size;
  /** Non-zero when body bytes were delivered to the route sink and are not retained here. */
  int body_streamed;
  int protocol_keep_alive;
  /** Borrowed portable TCP peer endpoint, available for network-backed server requests. */
  const cnet_stream_peer *peer;
  /** Verified TLS peer leaf SHA-256, or NULL for plaintext/no presented client certificate. */
  const char *peer_certificate_sha256;
  chttp_session *session;
  /** NULL unless JWT Bearer admission authenticated this callback. */
  const chttp_jwt_claims_view *jwt_claims;
} chttp_server_request_view;

/** Handler-scoped response builder. Memory replies are copied; source descriptors are retained. */
typedef struct chttp_server_response {
  void *impl;
} chttp_server_response;

/** Borrowed deferred response input copied before submission returns. */
typedef struct chttp_server_deferred_response {
  size_t size;
  unsigned int status_code;
  const char *content_type;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
} chttp_server_deferred_response;

typedef int (*chttp_server_handler_fn)(void *user, const chttp_server_request_view *request,
                                       chttp_server_response *response);

/**
 * Opens a bounded request-body sink after headers and route parameters are available.
 * Request pointers are borrowed only for this call; the returned sink descriptor is copied.
 */
typedef int (*chttp_server_body_open_fn)(void *user, const chttp_server_request_view *request,
                                         chttp_body_sink *out_sink);

/** Closes a route sink exactly once; status is SALTS_OK only after the complete body arrived. */
typedef void (*chttp_server_body_close_fn)(void *user, chttp_body_sink *sink, int status);

typedef int (*chttp_server_middleware_fn)(void *user, const chttp_server_request_view *request,
                                          chttp_server_response *response, chttp_server_next *next);

/** Copies Bearer validation configuration. Stop all users before destroying the validator. */
int chttp_jwt_bearer_validator_init(chttp_jwt_bearer_validator *validator,
                                    const chttp_jwt_bearer_validator_options *options);

/** Releases copied validation material. The validator must no longer be registered on a server. */
int chttp_jwt_bearer_validator_destroy(chttp_jwt_bearer_validator *validator);

typedef struct chttp_server_middleware {
  chttp_server_middleware_fn handler;
  void *user;
} chttp_server_middleware;

typedef struct chttp_server_route_options {
  chttp_method method;
  const char *path;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
  chttp_server_handler_fn handler;
  void *user;
  chttp_server_body_open_fn body_open;
  chttp_server_body_close_fn body_close;
} chttp_server_route_options;

/**
 * Runs as the terminal handler after global and route middleware approve an
 * HTTP/1.1 Upgrade or HTTP/2 Extended CONNECT. The request method is GET for
 * H1 and CONNECT for H2. Returning an error rejects with 500. Calling
 * `chttp_server_reply()` rejects with that in-memory reply; streamed and file
 * replies are invalid during admission. Otherwise the framework sends 101 for
 * H1 or 200 for H2. One frame may be admitted here and is retained until the
 * handshake response write completes.
 */
typedef int (*chttp_websocket_open_fn)(void *user, chttp_websocket *websocket,
                                       const chttp_server_request_view *request,
                                       chttp_server_response *response);

/** Runs serially on the server owner thread; event and peer are callback-scoped. */
typedef void (*chttp_websocket_event_fn)(void *user, chttp_websocket *websocket,
                                         const chttp_websocket_event *event);

typedef struct chttp_server_websocket_options {
  size_t size;
  const char *path;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
  /** Zero selects the largest payload that fits one bounded CNet send, capped at 64 KiB. */
  size_t max_frame_bytes;
  /** Zero selects max_frame_bytes. Must be at least max_frame_bytes. */
  size_t max_message_bytes;
  /** Zero selects max_frame_bytes plus the maximum RFC 6455 frame header. */
  size_t max_buffered_input_bytes;
  chttp_websocket_open_fn on_open;
  chttp_websocket_event_fn on_event;
  void *user;
} chttp_server_websocket_options;

/**
 * Every capacity is a hard bound. `network.connection_capacity` bounds active
 * accepted connections. When HTTP/2 is enabled, its stream, parser, output,
 * HPACK and SETTINGS limits are per connection. Routes are origin-form paths, may contain named
 * `:segment` parameters, and exclude the query string. The server invokes
 * handlers serially on its owner thread, so a handler must not block or call
 * stop/destroy. Session capacity zero disables Sessions; otherwise the Cookie
 * contains only a CSPRNG id and values stay in the bounded in-memory store.
 */
typedef struct chttp_server_config {
  const char *host;
  uint16_t port;
  size_t backlog;
  cnet_client_config network;
  size_t route_capacity;
  size_t middleware_capacity;
  size_t max_route_middleware_count;
  size_t max_route_param_count;
  size_t max_route_param_bytes;
  size_t max_target_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_request_body_bytes;
  size_t max_response_header_count;
  size_t max_response_header_bytes;
  size_t max_response_body_bytes;
  size_t session_capacity;
  size_t session_entry_capacity;
  size_t max_session_key_bytes;
  size_t max_session_value_bytes;
  uint32_t session_idle_timeout_ms;
  const char *session_cookie_name;
  int session_cookie_secure;
  uint32_t poll_slice_ms;
  /** Optional HTTPS/mTLS policy consumed during server initialization. */
  const cnet_tls_server_config *tls;
  /** Zero preserves the HTTP/1.1-only listener. One also accepts HTTP/2. */
  int enable_http2;
  /** Per-connection HTTP/2 stream hard bound and advertised concurrency limit. */
  size_t h2_stream_capacity;
  /** HTTP/2 parser input hard bound; must be at least 16,393 bytes. */
  size_t h2_input_buffer_bytes;
  /** H2 output bound; >=16,468, <=max_send_bytes, and large enough for response HPACK bounds. */
  size_t h2_output_buffer_bytes;
  /** HTTP/2 HPACK dynamic-table hard bound. */
  size_t h2_hpack_dynamic_table_bytes;
  /** Maximum SETTINGS entries accepted in one HTTP/2 frame. */
  size_t h2_max_settings_count;
  /** Per-response source chunk bound; zero selects up to 64 KiB within transport limits. */
  size_t stream_chunk_bytes;
  /** In-memory reply bound; zero derives the largest value fitting one transport send. */
  size_t max_buffered_response_body_bytes;
  /**
   * Aggregate bytes available to live request, response, transport and
   * WebSocket payload buffers. Zero preserves the legacy logical maximum but
   * allocates it only on demand. Exhaustion returns `SALTS_ENOBUFS`.
   */
  size_t buffer_capacity_bytes;
} chttp_server_config;

/** Socket policy copied into a stopped HTTP/WebSocket server before start. */
typedef struct chttp_server_socket_options {
  size_t size;
  cnet_stream_socket_options stream;
  cnet_listener_options listener;
} chttp_server_socket_options;

#define CHTTP_SERVER_SOCKET_OPTIONS_INIT                                                           \
  {sizeof(chttp_server_socket_options), CNET_STREAM_SOCKET_OPTIONS_INIT, CNET_LISTENER_OPTIONS_INIT}

/** Thread-safe snapshot of server lifecycle and bounded admission counters. */
typedef struct chttp_server_stats {
  uint16_t port;
  size_t active_connections;
  uint64_t accepted_connections;
  uint64_t rejected_connections;
  uint64_t requests;
  uint64_t responses;
  uint64_t protocol_errors;
  uint64_t handler_errors;
  size_t buffer_bytes;
  size_t peak_buffer_bytes;
  uint64_t rejected_buffer_allocations;
  int running;
  int stopping;
  int terminal_status;
} chttp_server_stats;

/**
 * Initializes a stopped server and copies configuration and bounded storage.
 * @return `SALTS_OK`, an invalid/range/aggregate-size error, or `SALTS_ENOMEM`.
 */
int chttp_server_init(chttp_server *server, const chttp_server_config *config);

/** Replaces socket policy before listener/network start; later calls return `SALTS_EBUSY`. */
int chttp_server_set_socket_options(chttp_server *server,
                                    const chttp_server_socket_options *options);

/** Optional absolute stage budgets in milliseconds. Zero disables the corresponding deadline. */
typedef struct chttp_server_deadlines {
  uint32_t headers_ms; /**< H1 message headers; H2 preface/frame/header-block assembly. */
  uint32_t body_ms; /**< Complete headers through admission/body/trailers. */
  uint32_t handler_ms; /**< Middleware/handler plus deferred completion; excludes response transfer. */
} chttp_server_deadlines;

/**
 * Copies budgets before start; returns SALTS_EINVAL for NULL, SALTS_EBUSY after start.
 * Expiry closes H1 or resets the affected H2 stream (incomplete H2 framing closes the connection).
 * Callbacks cannot be preempted. Deferred expiry only cancels an unclaimed PENDING terminal;
 * a worker already owning WRITING completes publication. No application side effects are undone.
 */
int chttp_server_set_deadlines(chttp_server *server, const chttp_server_deadlines *deadlines);

typedef struct chttp_server_admission_result {
  unsigned int status_code; /**< Zero continues; 400..599 rejects before body delivery. */
  uint32_t retry_after_seconds; /**< Optional Retry-After on a rejected request. */
} chttp_server_admission_result;

/**
 * Runs once after JWT on the owner thread, before body_open/100 Continue. Request params and
 * jwt_claims are available; body and Session are not. Views expire at callback return.
 * Return SALTS_OK with a decision; an error or invalid decision produces 500.
 * Must not block, reenter the server, or retain request pointers.
 */
typedef int (*chttp_server_admission_fn)(void *user, const chttp_server_request_view *request,
                                         chttp_server_admission_result *result);

/**
 * Sets one admission hook before start. NULL removes it. User remains borrowed until destroy.
 * Returns SALTS_OK, SALTS_EINVAL for an invalid server, or SALTS_EBUSY after start.
 */
int chttp_server_set_admission(chttp_server *server, chttp_server_admission_fn admission, void *user);

/**
 * Adds one method/path-pattern route before start. Complete `:name` segments
 * bind raw, non-percent-decoded params. The user pointer is borrowed through
 * stop. Returns `SALTS_ENOBUFS` at route/param capacity, `SALTS_EALREADY` for a
 * duplicate method/pattern, or `SALTS_EBUSY` after start.
 */
int chttp_server_route(chttp_server *server, chttp_method method, const char *path,
                       chttp_server_handler_fn handler, void *user);

int chttp_server_route_with(chttp_server *server, const chttp_server_route_options *options);

/** Registers one route protected by the supplied HS256 Bearer validator. */
int chttp_server_route_with_jwt_bearer(chttp_server *server,
                                       const chttp_server_route_options *options,
                                       chttp_jwt_bearer_validator *validator);

/** Installs one server-wide JWT Bearer admission policy before server start. */
int chttp_server_use_jwt_bearer(chttp_server *server, chttp_jwt_bearer_validator *validator);

int chttp_server_get(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                     void *user);

int chttp_server_head(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                      void *user);

int chttp_server_post(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                      void *user);

int chttp_server_put(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                     void *user);

int chttp_server_delete(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                        void *user);

int chttp_server_patch(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                       void *user);

int chttp_server_options(chttp_server *server, const char *path, chttp_server_handler_fn handler,
                         void *user);

/** Registers one explicit H1 Upgrade/H2 Extended CONNECT WebSocket route before server start. */
int chttp_server_websocket_with(chttp_server *server,
                                const chttp_server_websocket_options *options);

/** Registers one WebSocket route protected by the supplied JWT Bearer validator. */
int chttp_server_websocket_with_jwt_bearer(chttp_server *server,
                                           const chttp_server_websocket_options *options,
                                           chttp_jwt_bearer_validator *validator);

/** Convenience WebSocket route using bounded defaults and no route middleware. */
int chttp_server_websocket(chttp_server *server, const char *path, chttp_websocket_open_fn on_open,
                           chttp_websocket_event_fn on_event, void *user);

/** Server WebSocket operations are valid only from that peer's callbacks. */
int chttp_websocket_state_get(const chttp_websocket *websocket, chttp_websocket_state *out_state);

int chttp_websocket_send_text(chttp_websocket *websocket, const void *data, size_t size);

int chttp_websocket_send_binary(chttp_websocket *websocket, const void *data, size_t size);

int chttp_websocket_send_ping(chttp_websocket *websocket, const void *data, size_t size);

int chttp_websocket_send_pong(chttp_websocket *websocket, const void *data, size_t size);

int chttp_websocket_close(chttp_websocket *websocket, uint16_t code, const void *reason,
                          size_t reason_size);

/** Capture a stable server session value while inside on_open/on_event. */
int chttp_server_websocket_session_capture(const chttp_websocket *websocket,
                                           chttp_server_websocket_session *out_session);

/**
 * Thread-safe copied command admission for a captured server WebSocket.
 * SALTS_OK means the bounded server queue owns a copy; SALTS_ENOBUFS applies
 * backpressure and SALTS_ENOENT means the captured connection is no longer
 * current.
 */
int chttp_server_websocket_send_text(const chttp_server_websocket_session *session,
                                     const void *data, size_t size);

int chttp_server_websocket_send_binary(const chttp_server_websocket_session *session,
                                       const void *data, size_t size);

int chttp_server_websocket_send_ping(const chttp_server_websocket_session *session,
                                     const void *data, size_t size);

int chttp_server_websocket_send_pong(const chttp_server_websocket_session *session,
                                     const void *data, size_t size);

int chttp_server_websocket_close(const chttp_server_websocket_session *session, uint16_t code,
                                 const void *reason, size_t reason_size);

/**
 * Appends one global middleware before start. Bindings are copied in
 * registration order. Returns `SALTS_ENOBUFS` at middleware capacity.
 */
int chttp_server_use(chttp_server *server, chttp_server_middleware_fn middleware, void *user);

/** Immutable, application-owned CORS policy; retain it and its strings until server destruction. */
typedef struct chttp_server_cors_options {
  const char *const *origins; /**< Exact serialized origins, or a single "*" without credentials. */
  size_t origin_count;
  const char *methods; /**< Non-empty comma-separated, case-sensitive HTTP method tokens. */
  const char *allowed_headers; /**< Explicit header-name list, case-insensitive; NULL allows none. */
  const char *exposed_headers; /**< Explicit response header-name list, or NULL. */
  uint32_t max_age_seconds; /**< Preflight cache lifetime; zero disables caching. */
  int allow_credentials;
} chttp_server_cors_options;

/**
 * Registers global CORS middleware before start. Invalid policy returns SALTS_EINVAL;
 * registration also propagates chttp_server_use() errors. No configuration is copied.
 * Allowed preflights return 204, rejected origins/methods/headers return 403, malformed
 * preflight fields return 400. Requests without Origin continue normally.
 * JWT admission and streaming body delivery still precede ordinary middleware.
 */
int chttp_server_use_cors(chttp_server *server, const chttp_server_cors_options *options);

/** Runs the next middleware or terminal dispatch; a second call returns `SALTS_EALREADY`. */
int chttp_server_next_call(chttp_server_next *next);

/**
 * Starts the listener and background CNet owner thread. Port zero selects an
 * ephemeral port. Bind/backend/thread failures are returned before success.
 */
int chttp_server_start(chttp_server *server);

/** Returns the bound port after a successful start. */
int chttp_server_port(const chttp_server *server, uint16_t *out_port);

/** Stops admission and joins the owner thread. Timeout zero waits without a deadline. */
int chttp_server_stop(chttp_server *server, uint32_t timeout_ms);

/** Releases a stopped server; a zero server is already destroyed. */
int chttp_server_destroy(chttp_server *server);

/** Returns the first case-insensitive request header, or NULL. */
const char *chttp_server_request_header(const chttp_server_request_view *request, const char *name);

/** Returns one `:name` route parameter, or NULL. */
const char *chttp_server_request_param(const chttp_server_request_view *request, const char *name);

/**
 * Adds or replaces one copied response header within configured count/byte
 * bounds. Framing headers are framework-owned and return `SALTS_EPERM`.
 */
int chttp_server_response_set_header(chttp_server_response *response, const char *name,
                                     const char *value);

/**
 * Selects one case-sensitive WebSocket subprotocol token offered by the
 * current upgrade request. The selected token is copied into the handshake.
 */
int chttp_server_response_select_websocket_subprotocol(chttp_server_response *response,
                                                       const chttp_server_request_view *request,
                                                       const char *subprotocol);

/**
 * Seals the current regular HTTP/1.1 or HTTP/2 response and returns a cross-thread completion
 * handle. Request views remain callback-borrowed and must be copied by the
 * application before the handler returns. Existing response headers are
 * retained; response mutation after this call returns `SALTS_EALREADY`.
 *
 * H1 admits at most one deferred response per connection; H2 admits at most one
 * per configured stream slot. Total outstanding work is therefore bounded by
 * the configured connection and H2 stream capacities. Session-backed requests,
 * WebSocket handshakes, and deferred streaming bodies return `SALTS_ENOTSUP`.
 */
int chttp_server_response_defer(chttp_server_response *response,
                                chttp_server_deferred *out_deferred);

/**
 * Thread-safe terminal completion for a deferred response. Headers and body
 * are copied into configured CHTTP bounds before success; failure leaves the
 * handle pending for an explicit retry or cancel. A successful call clears the
 * handle and wakes the server owner. Server stop waits for every admitted
 * handle to complete.
 */
int chttp_server_deferred_reply(chttp_server_deferred *deferred,
                                const chttp_server_deferred_response *response);

/**
 * Cancels one pending deferred response without sending a replacement. Success
 * consumes the handle and aborts request state on the server owner. H1 closes
 * its exclusive connection because pipelined input cannot advance past the
 * missing response; H2 sends RST_STREAM(CANCEL) without failing sibling streams.
 *
 * This operation is thread-safe and generation checked. Exactly one concurrent
 * reply or cancel may claim the handle. A stale generation or owner-drained
 * handle returns `SALTS_ENOENT`. A matching generation held in `WRITING` by
 * another terminal operation returns `SALTS_EALREADY`; a failed reply restores
 * `PENDING`, while a successful terminal operation keeps returning
 * `SALTS_EALREADY` until the owner drains it. H2 RST_STREAM or peer close makes
 * a pending handle stale; a terminal call that already claimed `WRITING`
 * completes its bounded copy, which the owner then discards.
 */
int chttp_server_deferred_cancel(chttp_server_deferred *deferred);

/**
 * Completes the response with a copied content type and body. A second reply
 * returns `SALTS_EALREADY`; an oversized body returns `SALTS_EMSGSIZE`.
 */
int chttp_server_reply(chttp_server_response *response, unsigned int status_code,
                       const char *content_type, const void *body, size_t body_size);

/**
 * Streams a response after the handler returns. The source descriptor is copied, but its user
 * state must remain valid until EOF or connection/stream cancellation.
 */
int chttp_server_response_source(chttp_server_response *response, unsigned int status_code,
                                 const char *content_type, const chttp_body_source *source);

/** Streams a regular file through the server's shared asynchronous file runtime. */
int chttp_server_response_file(chttp_server_response *response, unsigned int status_code,
                               const char *content_type, const char *path);

typedef struct chttp_server_file_options {
  const char *path;
  /** NULL selects a MIME type from the extension, or application/octet-stream. */
  const char *content_type;
  /** Optional quoted entity-tag. Strong tags must identify an immutable version. */
  const char *etag;
} chttp_server_file_options;

/**
 * Serves an application-selected regular file for GET/HEAD, with conditional requests and
 * one byte range. Inputs are borrowed until return; the file must remain immutable until
 * completion. No URL-to-filesystem mapping is performed. Metadata generates a weak ETag
 * unless options supplies one. If-Range requires a matching strong application ETag;
 * other validators cause a complete response. Unsupported/malformed range syntax is ignored.
 * Returns SALTS_OK on response admission (including 304/405/412/416), or a Salts error;
 * filesystem errors are returned to the handler. Header/body capacity limits still apply.
 */
int chttp_server_serve_file(chttp_server_response *response,
    const chttp_server_request_view *request, const chttp_server_file_options *options);

/**
 * Session values are NUL-terminated strings borrowed through the handler and
 * copied into the bounded store by set. A first set lazily allocates a Session;
 * a full live-session or entry capacity returns `SALTS_ENOBUFS`.
 */
const char *chttp_session_get(const chttp_session *session, const char *key);

int chttp_session_set(chttp_session *session, const char *key, const char *value);

int chttp_session_remove(chttp_session *session, const char *key);

int chttp_session_clear(chttp_session *session);

int chttp_session_invalidate(chttp_session *session);

/** Obtains a thread-safe stats snapshot without advancing server state. */
int chttp_server_get_stats(const chttp_server *server, chttp_server_stats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_HTTP_H */
