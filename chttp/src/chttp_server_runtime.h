#ifndef CHTTP_SERVER_RUNTIME_H
#define CHTTP_SERVER_RUNTIME_H

#include "chttp_file_transfer.h"
#include "chttp_server_internal.h"

#include <cnet/websocket.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct chttp_server_impl chttp_server_impl;
typedef struct chttp_server_connection chttp_server_connection;
typedef struct chttp_server_request_state chttp_server_request_state;
typedef struct chttp_h2_server_connection chttp_h2_server_connection;
typedef struct chttp_server_deferred_target chttp_server_deferred_target;

extern SALTS_THREAD_LOCAL chttp_server_impl *chttp_active_callback_server;

typedef enum chttp_server_wire_protocol {
  CHTTP_SERVER_WIRE_UNKNOWN = 0,
  CHTTP_SERVER_WIRE_HTTP_1_1,
  CHTTP_SERVER_WIRE_HTTP_2
} chttp_server_wire_protocol;

typedef struct chttp_server_route_record {
  chttp_method method;
  char *path;
  chttp_server_middleware *middleware;
  size_t middleware_count;
  size_t param_count;
  chttp_server_handler_fn handler;
  void *user;
  chttp_server_body_open_fn body_open;
  chttp_server_body_close_fn body_close;
  chttp_jwt_bearer_validator *jwt_bearer_validator;
  chttp_websocket_open_fn websocket_open;
  chttp_websocket_event_fn websocket_event;
  size_t websocket_max_frame_bytes;
  size_t websocket_max_message_bytes;
  size_t websocket_max_buffered_input_bytes;
  void *websocket_user;
  bool websocket;
  bool dynamic;
} chttp_server_route_record;

#include "chttp_websocket_server_internal.h"

typedef struct chttp_server_response_builder {
  chttp_server_impl *server;
  chttp_server_connection *connection;
  chttp_server_deferred_target *deferred_target;
  const chttp_server_request_view *request;
  chttp_header *headers;
  char *header_storage;
  unsigned char *body;
  size_t header_capacity;
  size_t header_storage_capacity;
  size_t body_capacity;
  size_t body_limit;
  size_t source_capacity;
  size_t header_count;
  size_t header_storage_used;
  size_t header_wire_bytes;
  size_t body_size;
  size_t source_transferred;
  unsigned int status_code;
  chttp_body_source body_source;
  void (*source_cleanup)(void *user, int status);
  void *source_cleanup_user;
  chttp_file_transfer *file_transfer;
  bool replied;
  bool source_enabled;
  bool deferred;
} chttp_server_response_builder;

typedef struct chttp_session_entry {
  char *key;
  char *value;
  bool used;
} chttp_session_entry;

typedef struct chttp_session_record {
  char id[33];
  chttp_session_entry *entries;
  uint64_t expires_at_ms;
  bool used;
} chttp_session_record;

typedef struct chttp_session_context {
  chttp_server_impl *server;
  chttp_session_record *record;
  bool presented;
  bool created;
  bool invalidated;
} chttp_session_context;

struct chttp_server_request_state {
  chttp_server_impl *server;
  chttp_server_response_builder response_builder;
  chttp_server_response response;
  chttp_server_param *params;
  char *param_storage;
  size_t param_storage_capacity;
  size_t param_storage_used;
  size_t param_count;
  chttp_session session;
  chttp_session_context session_context;
  void *jwt_owner;
  chttp_jwt_claims_view jwt_claims;
  chttp_server_route_record *admitted_route;
  unsigned int admitted_allowed_methods;
  unsigned int admitted_fallback_status;
  bool admission_complete;
  bool admission_rejected;
  chttp_server_route_record *body_route;
  chttp_body_sink body_sink;
  bool body_sink_open;
  bool body_was_streamed;
};

typedef struct chttp_server_chain {
  chttp_server_impl *server;
  chttp_server_request_state *request_state;
  const chttp_server_request_view *request;
  chttp_server_response *response;
  chttp_server_route_record *route;
  unsigned int fallback_status;
  unsigned int allowed_methods;
  chttp_server_handler_fn terminal;
  void *terminal_user;
} chttp_server_chain;

typedef struct chttp_server_next_impl {
  chttp_server_chain *chain;
  size_t index;
  bool called;
} chttp_server_next_impl;

typedef enum chttp_server_pending_action {
  CHTTP_SERVER_PENDING_NONE = 0,
  CHTTP_SERVER_PENDING_RECEIVE,
  CHTTP_SERVER_PENDING_SEND,
  CHTTP_SERVER_PENDING_CLOSE
} chttp_server_pending_action;

typedef enum chttp_server_deferred_state {
  CHTTP_SERVER_DEFERRED_IDLE = 0,
  CHTTP_SERVER_DEFERRED_PENDING,
  CHTTP_SERVER_DEFERRED_WRITING,
  CHTTP_SERVER_DEFERRED_READY,
  CHTTP_SERVER_DEFERRED_CANCELED
} chttp_server_deferred_state;

#define CHTTP_SERVER_DEFERRED_TOKEN_STATE_BITS 32u

static inline uint_fast64_t chttp_server_deferred_token(uint32_t generation,
                                                        chttp_server_deferred_state state) {
  return ((uint_fast64_t)generation << CHTTP_SERVER_DEFERRED_TOKEN_STATE_BITS) |
         (uint_fast64_t)(uint32_t)state;
}

static inline uint32_t chttp_server_deferred_token_generation(uint_fast64_t token) {
  return (uint32_t)(token >> CHTTP_SERVER_DEFERRED_TOKEN_STATE_BITS);
}

static inline chttp_server_deferred_state chttp_server_deferred_token_state(uint_fast64_t token) {
  return (chttp_server_deferred_state)(uint32_t)token;
}

static inline int chttp_server_deferred_claim(atomic_uint_fast64_t *token, uint32_t generation) {
  uint_fast64_t expected = chttp_server_deferred_token(generation, CHTTP_SERVER_DEFERRED_PENDING);
  if (atomic_compare_exchange_strong_explicit(
          token, &expected, chttp_server_deferred_token(generation, CHTTP_SERVER_DEFERRED_WRITING),
          memory_order_acq_rel, memory_order_acquire))
    return SALTS_OK;
  return chttp_server_deferred_token_generation(expected) != generation ||
                 chttp_server_deferred_token_state(expected) == CHTTP_SERVER_DEFERRED_IDLE
             ? SALTS_ENOENT
             : SALTS_EALREADY;
}

typedef enum chttp_server_deferred_kind {
  CHTTP_SERVER_DEFERRED_HTTP_1_1 = 1,
  CHTTP_SERVER_DEFERRED_HTTP_2
} chttp_server_deferred_kind;

struct chttp_server_deferred_target {
  chttp_server_impl *server;
  chttp_server_connection *connection;
  chttp_server_request_state *request_state;
  chttp_server_response_builder *response_builder;
  chttp_server_response *response;
  atomic_uint_fast64_t *token;
  chttp_server_request_view request;
  chttp_server_deferred_kind kind;
};

typedef enum chttp_server_websocket_command_kind {
  CHTTP_SERVER_WEBSOCKET_COMMAND_TEXT = 1,
  CHTTP_SERVER_WEBSOCKET_COMMAND_BINARY,
  CHTTP_SERVER_WEBSOCKET_COMMAND_PING,
  CHTTP_SERVER_WEBSOCKET_COMMAND_PONG,
  CHTTP_SERVER_WEBSOCKET_COMMAND_CLOSE
} chttp_server_websocket_command_kind;

typedef struct chttp_server_websocket_command {
  chttp_server_websocket_session session;
  unsigned char *data;
  size_t size;
  uint16_t close_code;
  chttp_server_websocket_command_kind kind;
} chttp_server_websocket_command;

struct chttp_server_connection {
  chttp_server_impl *server;
  cnet_connection handle;
  chttp_server_parser parser;
  chttp_server_request_state request_state;
  chttp_server_response_builder deferred_builder;
  chttp_server_response deferred_response;
  chttp_server_deferred_target deferred_target;
  chttp_h2_server_connection *h2;
  chttp_server_websocket_peer websocket_peer;
  unsigned char *websocket_upgrade_input;
  size_t websocket_upgrade_input_capacity;
  size_t websocket_upgrade_input_size;
  unsigned char *outbound;
  size_t outbound_capacity;
  size_t outbound_size;
  uint64_t h2_close_after_ms;
  unsigned char protocol_prefix[24];
  size_t protocol_prefix_size;
  chttp_server_wire_protocol wire_protocol;
  cnet_stream_peer peer;
  char peer_certificate_sha256[CNET_TLS_PEER_CERTIFICATE_SHA256_CAPACITY];
  atomic_uint_fast64_t deferred_token;
  bool active;
  bool connected;
  bool writing;
  bool close_after_write;
  bool response_streaming;
  bool response_source_chunked;
  bool response_close_after_stream;
  bool deferred_disconnected;
  bool deferred_response_writing;
  chttp_server_pending_action pending_action;
};

struct chttp_server_impl {
  chttp_server_config config;
  chttp_server_socket_options socket_options;
  char *host;
  char *session_cookie_name;
  chttp_server_route_record *routes;
  char *route_paths;
  chttp_server_middleware *route_middleware;
  chttp_server_middleware *middleware;
  chttp_jwt_bearer_validator *jwt_bearer_validator;
  size_t route_count;
  size_t middleware_count;
  size_t max_response_wire_bytes;
  size_t pending_retry_cursor;
  chttp_server_connection *connections;
  chttp_session_record *sessions;
  chttp_session_entry *session_entries;
  char *session_keys;
  char *session_values;
  cnet_client network;
  cnet_listener listener;
  cnet_tls_server tls_server;
  cflow_io_file_runtime file_runtime;
  chttp_file_transfer **file_transfers;
  size_t file_transfer_capacity;
  chttp_server_websocket_command *websocket_commands;
  size_t websocket_command_head;
  size_t websocket_command_count;
  salts_mutex_t mutex;
  salts_cond_t changed;
  salts_thread_t thread;
  chttp_server_stats stats;
  atomic_size_t buffer_bytes;
  atomic_size_t peak_buffer_bytes;
  _Atomic uint64_t rejected_buffer_allocations;
  bool sync_initialized;
  bool network_initialized;
  bool listener_initialized;
  bool tls_initialized;
  bool thread_started;
  bool start_called;
  bool stop_requested;
  bool worker_done;
  bool file_runtime_initialized;
};

int chttp_server_response_builder_init(chttp_server_response_builder *builder,
                                       const chttp_server_config *config);
void chttp_server_response_builder_reset(chttp_server_response_builder *builder);
void chttp_server_response_builder_destroy(chttp_server_response_builder *builder);
void chttp_server_response_builder_close_source(chttp_server_response_builder *builder, int status);
int chttp_server_response_source_owned(chttp_server_response *response, unsigned int status_code,
                                       const char *content_type, const chttp_body_source *source,
                                       void (*cleanup)(void *user, int status), void *cleanup_user);
int chttp_server_response_serialize(const chttp_server_response_builder *builder,
                                    const chttp_server_request_view *request, unsigned char *output,
                                    size_t output_capacity, size_t *inout_size);
int chttp_server_error_serialize(unsigned int status_code, unsigned char *output,
                                 size_t output_capacity, size_t *inout_size);

int chttp_server_request_state_init(chttp_server_request_state *state, chttp_server_impl *server);
void chttp_server_request_state_reset(chttp_server_request_state *state);
void chttp_server_request_state_destroy(chttp_server_request_state *state);
int chttp_server_request_admit(chttp_server_request_state *state,
                               const chttp_server_request_view *request, chttp_method route_method);
int chttp_server_dispatch_request(chttp_server_request_state *state,
                                  const chttp_server_request_view *request);
int chttp_server_request_body_open(chttp_server_request_state *state,
                                   const chttp_server_request_view *request,
                                   chttp_body_sink *out_sink);
int chttp_server_request_body_write(chttp_server_request_state *state, const void *data,
                                    size_t size);
void chttp_server_request_body_close(chttp_server_request_state *state, int status);
bool chttp_server_request_body_streaming(const chttp_server_request_state *state);

int chttp_server_route_register(chttp_server_impl *server,
                                const chttp_server_route_options *options);
int chttp_server_websocket_route_register(chttp_server_impl *server,
                                          const chttp_server_websocket_options *options);
chttp_server_route_record *chttp_server_route_find(chttp_server_request_state *state,
                                                   chttp_method method, const char *path,
                                                   unsigned int *out_allowed_methods,
                                                   int *out_status);
int chttp_server_chain_run(chttp_server_chain *chain);

int chttp_server_websocket_upgrade(void *user, const chttp_server_request_view *request,
                                   chttp_server_parser_upgrade_action *out_action,
                                   unsigned int *out_http_status);
int chttp_server_websocket_input(chttp_server_connection *connection, const void *data,
                                 size_t size);
int chttp_server_websocket_send_complete(chttp_server_connection *connection);
void chttp_server_websocket_transport_closed(chttp_server_connection *connection);
void chttp_server_websocket_reset(chttp_server_connection *connection);
int chttp_server_websocket_commands_progress(chttp_server_impl *server);

int chttp_session_store_init(chttp_server_impl *server);
void chttp_session_store_destroy(chttp_server_impl *server);
void chttp_session_request_begin(chttp_server_request_state *state,
                                 const chttp_server_request_view *request);
int chttp_session_request_finish(chttp_server_request_state *state);
void chttp_session_request_abort(chttp_server_request_state *state);

void chttp_server_stats_connection_open(chttp_server_impl *server);
void chttp_server_stats_connection_close(chttp_server_impl *server);
void chttp_server_stats_request(chttp_server_impl *server);
void chttp_server_stats_response(chttp_server_impl *server);
void chttp_server_stats_protocol_error(chttp_server_impl *server);
void chttp_server_stats_handler_error(chttp_server_impl *server);

int chttp_server_file_runtime_ensure(chttp_server_impl *server,
                                     cflow_io_file_runtime **out_runtime);
int chttp_server_file_transfer_register(chttp_server_impl *server, chttp_file_transfer *transfer);
int chttp_server_send_pending(chttp_server_connection *connection);
int chttp_server_buffer_grow(void *context, unsigned char **buffer, size_t *capacity,
                             size_t required, size_t limit, size_t preserve_size);
void chttp_server_buffer_release(void *context, unsigned char *buffer, size_t capacity);
int chttp_server_connection_reserve_outbound(chttp_server_connection *connection, size_t required);
void chttp_server_connection_release_outbound(chttp_server_connection *connection);
void chttp_server_connection_close(chttp_server_connection *connection);
void chttp_server_request_enrich(const chttp_server_connection *connection,
                                 chttp_server_request_view *request);

#endif /* CHTTP_SERVER_RUNTIME_H */
