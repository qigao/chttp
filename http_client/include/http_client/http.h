#ifndef HTTP_CLIENT_HTTP_H
#define HTTP_CLIENT_HTTP_H

#include <http_common/http.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Blocking requests-style HTTP/1 client; one instance admits one call at a time. */
typedef struct chttp_client {
  void *impl;
} chttp_client;

/** Advanced caller-driven client for RPC, executors, and event-loop adapters. */
typedef struct chttp_async_client {
  void *impl;
} chttp_async_client;

/** Single-owner requests-style WebSocket client; public calls drive CNet internally. */
typedef struct chttp_websocket_client {
  void *impl;
} chttp_websocket_client;

/** Single-owner RFC 8441 session pool; public calls drive one shared H2 connection internally. */
typedef struct chttp_websocket_pool {
  void *impl;
} chttp_websocket_pool;

/** Generation-checked WebSocket stream handle; never a pointer or CNet handle. */
typedef struct chttp_websocket_session {
  uint32_t slot;
  uint32_t generation;
} chttp_websocket_session;

/** Generation-checked request handle; never a pointer or CNet handle. */
typedef struct chttp_request {
  uint32_t slot;
  uint32_t generation;
} chttp_request;

/** Formats a borrowed Authorization header in caller-owned storage. */
int chttp_jwt_bearer_header(const char *token, char *buffer, size_t buffer_size,
                            chttp_header *out_header);

typedef struct chttp_websocket_client_config {
  size_t size;
  cnet_client_config network;
  size_t max_frame_bytes;
  size_t max_message_bytes;
  size_t max_buffered_input_bytes;
  size_t max_handshake_header_bytes;
  /** Completed event queue hard bound; zero selects network.event_capacity. */
  size_t event_capacity;
  /** HTTP/2 parser input hard bound; zero selects 128 KiB. */
  size_t h2_input_buffer_bytes;
  /** HTTP/2 HPACK dynamic table hard bound; zero selects 4096 bytes. */
  size_t h2_hpack_dynamic_table_bytes;
  /** HTTP/2 SETTINGS entries accepted per frame; zero selects 16. */
  size_t h2_max_settings_count;
  /** Optional CNet socket policy; zeroed size preserves platform defaults. */
  cnet_stream_socket_options socket_options;
} chttp_websocket_client_config;

typedef struct chttp_websocket_connect_options {
  size_t size;
  /** Complete ws:// or wss:// URI, including target path and optional query. */
  const char *uri;
  /** Extra copied HTTP headers; handshake-owned fields cannot be overridden. */
  const chttp_header *headers;
  size_t header_count;
  /** Required for custom WSS trust; invalid for ws:// and ALPN must match `protocol`. */
  const chttp_tls_profile *tls;
  uint32_t timeout_ms;
  /** HTTP/1.1 Upgrade by default; HTTP/2 selects RFC 8441 Extended CONNECT. */
  chttp_protocol protocol;
  /** Optional single protocol token; the server must select it exactly. */
  const char *subprotocol;
} chttp_websocket_connect_options;

typedef struct chttp_websocket_pool_config {
  size_t size;
  /** Shared transport limits plus the per-session WebSocket and event bounds. */
  chttp_websocket_client_config client;
  /** Concurrent RFC 8441 streams on the one physical H2 connection. */
  size_t session_capacity;
} chttp_websocket_pool_config;

/** Borrowed response view valid only for the duration of an advanced completion callback. */
typedef struct chttp_response_view {
  unsigned int http_major;
  unsigned int http_minor;
  unsigned int status_code;
  const char *reason;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  int protocol_keep_alive;
} chttp_response_view;

/** Owning response returned by a blocking requests-style method. */
typedef struct chttp_response {
  unsigned int http_major;
  unsigned int http_minor;
  unsigned int status_code;
  char *reason;
  chttp_header *headers;
  size_t header_count;
  void *body;
  size_t body_size;
  int protocol_keep_alive;
} chttp_response;

/** `stage` is a stable library-owned string. */
typedef struct chttp_error {
  int status;
  int native_status;
  const char *stage;
} chttp_error;

typedef void (*chttp_complete_fn)(void *user, chttp_request request,
                                  const chttp_response_view *response, const chttp_error *error);

/**
 * One admitted request owns one bounded response state. HTTP/1.1 exclusively
 * leases a pooled CNet connection; HTTP/2 leases one stream from a multiplexed
 * session. `connection_uri` accepts `tcp://` and `tls://` for both protocols,
 * plus `pipe://` for HTTP/1.1. `authority` becomes Host or `:authority`, while
 * `target` is an origin-form target beginning with `/` (or `*` for OPTIONS).
 * Header/body/options are copied before submit succeeds. `user` remains
 * borrowed until the terminal callback returns.
 */
typedef struct chttp_request_options {
  const char *connection_uri;
  const char *authority;
  const char *target;
  chttp_method method;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  /** Borrowed through the terminal callback; mutually exclusive with `body/body_size`. */
  const chttp_body_source *body_source;
  /** Borrowed through the terminal callback; streamed responses expose `body == NULL`. */
  const chttp_body_sink *body_sink;
  chttp_complete_fn on_complete;
  void *user;
  /** Optional reusable TLS profile; valid only with a `tls://` connection URI. */
  const chttp_tls_profile *tls;
  /** Explicit wire protocol. HTTP/2 never falls back to HTTP/1.1. */
  chttp_protocol protocol;
} chttp_request_options;

/**
 * Inputs for one blocking requests-style call. All inputs are copied before asynchronous
 * progress begins. `timeout_ms` bounds the wait for an HTTP result; zero
 * disables it and leaves termination to the configured transport deadlines.
 * After that deadline, terminal cancellation/drain may continue before the
 * function returns so the client is safe to reuse or destroy.
 */
typedef struct chttp_options {
  const char *connection_uri;
  const char *authority;
  const char *target;
  const chttp_header *headers;
  size_t header_count;
  const void *body;
  size_t body_size;
  /** Borrowed for the duration of this blocking call; mutually exclusive with `body/body_size`. */
  const chttp_body_source *body_source;
  /** Borrowed for the duration of this blocking call; streamed responses expose `body == NULL`. */
  const chttp_body_sink *body_sink;
  uint32_t timeout_ms;
  /** Optional reusable TLS profile; valid only with a `tls://` connection URI. */
  const chttp_tls_profile *tls;
  /** Explicit wire protocol. HTTP/2 never falls back to HTTP/1.1. */
  chttp_protocol protocol;
} chttp_options;

/**
 * All capacities are hard bounds. CHTTP uses strict llhttp parsing, buffers one
 * complete response, and admits at most one HTTP/1.1 request at a time per
 * connection. `request_capacity` bounds request slots and HTTP/2 streams;
 * `network.connection_capacity` independently bounds live physical H1
 * connections and H2 sessions, so it may be smaller than `request_capacity`.
 * `network.read_timeout_ms` applies while an idle connection observes its
 * peer. Serialized requests are additionally bounded by
 * `network.max_send_bytes`.
 */
typedef struct chttp_client_config {
  cnet_client_config network;
  size_t request_capacity;
  size_t max_start_line_bytes;
  size_t max_header_count;
  size_t max_header_bytes;
  size_t max_request_body_bytes;
  size_t max_response_body_bytes;
  size_t max_informational_responses;
  /** Per-request source buffer bound; zero selects up to 64 KiB within transport limits. */
  size_t stream_chunk_bytes;
  /** HTTP/2 parser input hard bound; zero selects 128 KiB or the larger CNet receive buffer. */
  size_t h2_input_buffer_bytes;
  /** HTTP/2 HPACK dynamic-table hard bound; zero selects 4 KiB. */
  size_t h2_hpack_dynamic_table_bytes;
  /** Maximum SETTINGS entries accepted in one HTTP/2 frame; zero selects 32. */
  size_t h2_max_settings_count;
} chttp_client_config;

/**
 * Initializes one advanced caller-driven CHTTP/CNet owner. No partial client
 * is published.
 * @param client Zero-initialized output owner.
 * @param config Borrowed configuration copied during initialization.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_ENOMEM`, or a CNet/backend init error.
 */
int chttp_async_client_init(chttp_async_client *client, const chttp_client_config *config);

/**
 * Serializes and copies a request, then reuses an H1 connection or H2 session
 * keyed by exact `connection_uri + authority + TLS profile + protocol`, or
 * asynchronously connects through CNet.
 * Success guarantees exactly one later completion callback. No callback is
 * delivered for immediate admission failure. Submission from a completion
 * callback returns `SALTS_EBUSY`; defer it until the callback unwinds.
 * A full pool may begin closing one non-matching idle connection and returns
 * `SALTS_ENOBUFS`; poll before retrying admission.
 * @return `SALTS_OK`, an input/size/transport error, `SALTS_ENOBUFS`,
 * `SALTS_EBUSY`, `SALTS_ESHUTDOWN`, or a CNet admission error.
 */
int chttp_async_client_submit(chttp_async_client *client, const chttp_request_options *options,
                              chttp_request *out_request);

/**
 * Requests cancellation; completion is reported later with `SALTS_ECANCELED`.
 * H1 closes its exclusive connection; H2 sends RST_STREAM(CANCEL) without
 * closing sibling streams. A completed request is stale and returns
 * `SALTS_ENOENT` after recycling.
 * @return `SALTS_OK`, `SALTS_ENOENT`, `SALTS_EALREADY`, or a CNet close error.
 */
int chttp_async_request_cancel(chttp_async_client *client, chttp_request request);

/**
 * Advanced integration API. Advances CNet and HTTP parsing on the calling
 * thread. Ordinary callers should use chttp_get/post/put and never call this
 * function. `out_completions` counts user completion callbacks, not transport
 * callbacks.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_EBUSY`, `SALTS_ESHUTDOWN`,
 * or the first CNet/progress error.
 */
int chttp_async_client_poll(chttp_async_client *client, uint32_t timeout_ms,
                            size_t *out_completions);

/**
 * Stops admission and drains all accepted requests plus busy and idle CNet connections.
 * Retry after `SALTS_ETIMEDOUT`; calling from poll/callback returns `SALTS_EBUSY`.
 */
int chttp_async_client_stop(chttp_async_client *client, uint32_t timeout_ms);

/**
 * Requires a completed stop; a null implementation is already destroyed.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_EBUSY`, or a CNet destroy error.
 */
int chttp_async_client_destroy(chttp_async_client *client);

/** Returns the first case-insensitive matching response header, or NULL. */
const char *chttp_response_view_header(const chttp_response_view *response, const char *name);

/**
 * Initializes an ordinary sequential requests-style client. Calls drive CNet
 * internally, including bounded idle-origin eviction; callers do not provide
 * a poller, executor, or worker thread.
 */
int chttp_client_init(chttp_client *client, const chttp_client_config *config);

/** Blocking requests-style methods returning an owning response. */
int chttp_get(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error);

int chttp_head(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error);

int chttp_post(chttp_client *client, const chttp_options *options, chttp_response *out_response,
               chttp_error *out_error);

int chttp_put(chttp_client *client, const chttp_options *options, chttp_response *out_response,
              chttp_error *out_error);

int chttp_delete(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                 chttp_error *out_error);

int chttp_patch(chttp_client *client, const chttp_options *options, chttp_response *out_response,
                chttp_error *out_error);

/**
 * Blocking requests-style file uploads using an exact Content-Length obtained
 * from `path`. File reads are submitted through the client's private shared
 * asynchronous file runtime while this call drives network and file progress.
 * File and progress callback state is confined to the call. `progress` may be
 * NULL; when present it observes monotonically increasing byte counts.
 */
int chttp_post_file(chttp_client *client, const chttp_options *options, const char *path,
                    chttp_progress_fn progress, void *progress_user, chttp_response *out_response,
                    chttp_error *out_error);

int chttp_put_file(chttp_client *client, const chttp_options *options, const char *path,
                   chttp_progress_fn progress, void *progress_user, chttp_response *out_response,
                   chttp_error *out_error);

/**
 * Streams a GET response through native asynchronous writes into a
 * same-directory temporary file. A 2xx response is fsynced, closed, then
 * atomically renamed over `output_path`; HTTP errors keep the owning response
 * but leave the destination unchanged.
 */
int chttp_download_file(chttp_client *client, const chttp_options *options, const char *output_path,
                        chttp_progress_fn progress, void *progress_user,
                        chttp_response *out_response, chttp_error *out_error);

/** Returns the first case-insensitive matching owning response header. */
const char *chttp_response_header(const chttp_response *response, const char *name);

/** Releases every allocation owned by a blocking response; zero is accepted. */
void chttp_response_destroy(chttp_response *response);

/**
 * Stops and drains the internal client. `SALTS_ETIMEDOUT` is retryable and
 * preserves the client; successful destroy clears `client->impl`.
 */
int chttp_client_destroy(chttp_client *client, uint32_t timeout_ms);

/**
 * Initializes a disconnected, single-owner requests-style client. Every
 * capacity is a hard bound; zero WebSocket limits select bounded defaults.
 */
int chttp_websocket_client_init(chttp_websocket_client *client,
                                const chttp_websocket_client_config *config);

/** Connects through HTTP/1.1 Upgrade or RFC 8441 Extended CONNECT. No protocol fallback occurs. */
int chttp_websocket_client_connect(chttp_websocket_client *client,
                                   const chttp_websocket_connect_options *options,
                                   unsigned int *out_http_status);

/** Blocking sends; callers never drive a poller. The client is not concurrently callable. */
int chttp_websocket_client_send_text(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms);

int chttp_websocket_client_send_binary(chttp_websocket_client *client, const void *data,
                                       size_t size, uint32_t timeout_ms);

int chttp_websocket_client_send_ping(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms);

int chttp_websocket_client_send_pong(chttp_websocket_client *client, const void *data, size_t size,
                                     uint32_t timeout_ms);

/** Returns one borrowed event view, valid until the next client operation. */
int chttp_websocket_client_receive(chttp_websocket_client *client, uint32_t timeout_ms,
                                   chttp_websocket_event *out_event);

/** Performs the close handshake within the deadline, then closes the transport. */
int chttp_websocket_client_close(chttp_websocket_client *client, uint16_t code, const void *reason,
                                 size_t reason_size, uint32_t timeout_ms);

/** Drains and releases CNet; active connections are closed within timeout_ms. */
int chttp_websocket_client_destroy(chttp_websocket_client *client, uint32_t timeout_ms);

/**
 * Initializes a disconnected HTTP/2 WebSocket pool. All capacities are hard bounds and storage is
 * reserved before success. The pool is single-owner and not concurrently callable.
 */
int chttp_websocket_pool_init(chttp_websocket_pool *pool,
                              const chttp_websocket_pool_config *config);

/**
 * Opens one RFC 8441 stream without exposing a poller. The first call fixes the connection origin
 * and TLS profile; later calls may change only the URI target. Local/peer stream exhaustion returns
 * `SALTS_ENOBUFS`.
 */
int chttp_websocket_pool_open(chttp_websocket_pool *pool,
                              const chttp_websocket_connect_options *options,
                              chttp_websocket_session *out_session, unsigned int *out_http_status);

int chttp_websocket_pool_send_text(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms);

int chttp_websocket_pool_send_binary(chttp_websocket_pool *pool, chttp_websocket_session session,
                                     const void *data, size_t size, uint32_t timeout_ms);

int chttp_websocket_pool_send_ping(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms);

int chttp_websocket_pool_send_pong(chttp_websocket_pool *pool, chttp_websocket_session session,
                                   const void *data, size_t size, uint32_t timeout_ms);

/** Returns one borrowed event view, valid until the next operation on this pool. */
int chttp_websocket_pool_receive(chttp_websocket_pool *pool, chttp_websocket_session session,
                                 uint32_t timeout_ms, chttp_websocket_event *out_event);

/** Closes and releases only this HTTP/2 stream; sibling sessions remain usable. */
int chttp_websocket_pool_close(chttp_websocket_pool *pool, chttp_websocket_session session,
                               uint16_t code, const void *reason, size_t reason_size,
                               uint32_t timeout_ms);

/** Closes active streams, then drains and releases the one shared CNet connection. */
int chttp_websocket_pool_destroy(chttp_websocket_pool *pool, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_HTTP_H */
