#ifndef CHTTP_SERVER_ENGINE_H
#define CHTTP_SERVER_ENGINE_H
/* Private protocol-engine control plane. Business applications use CHttp::App. */
#include <http_server/http.h>
#include <http_server/rpc.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private Application deferred-buffered response observer. The caller owns
 * address-stable ticket storage until complete returns. Callback runs on the
 * HTTP owner and must only record/signal, never execute application work.
 * H2 success follows the CNet send that contains the final END_STREAM bytes,
 * not the protocol encoder's earlier on_stream_close event. */
typedef struct chttp_server_response_terminal {
  void (*complete)(void *, int);
  void *user;
  void *owner; /* Noncopyable registration; caller zero-initializes the ticket. */
  struct chttp_server_response_terminal *next;
  int32_t stream_id;
} chttp_server_response_terminal;
int chttp_server_response_observe(chttp_server_response *, chttp_server_response_terminal *);
int chttp_server_response_unobserve(chttp_server_response *, chttp_server_response_terminal *);

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
  const cmeta_function_desc *operation;
} chttp_server_route_options;

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

int chttp_server_next_call(chttp_server_next *next);

int chttp_server_rpc_configure(chttp_server *server,
                              const chttp_server_rpc_config *config);

int chttp_server_rpc_register(chttp_server *server, const char *target,
                              const crpc_method *method,
                              crpc_server_method_fn handler, void *user);

int chttp_server_set_owner_placement(
    chttp_server *server, const chttp_server_owner_placement_options *options);

int chttp_server_set_websocket_transport(
    chttp_server *server, const chttp_server_websocket_transport_options *options);

int chttp_server_init(chttp_server *server, const chttp_server_config *config);

int chttp_server_set_socket_options(chttp_server *server,
                                    const chttp_server_socket_options *options);

int chttp_server_set_execution_options(
    chttp_server *server, const chttp_server_execution_options *options);

int chttp_server_set_deadlines(chttp_server *server, const chttp_server_deadlines *deadlines);

int chttp_server_set_admission(chttp_server *server, chttp_server_admission_fn admission, void *user);

int chttp_server_route(chttp_server *server, chttp_method method, const char *path,
                       chttp_server_handler_fn handler, void *user);

int chttp_server_route_with(chttp_server *server, const chttp_server_route_options *options);

int chttp_server_route_with_jwt_bearer(chttp_server *server,
                                       const chttp_server_route_options *options,
                                       chttp_jwt_bearer_validator *validator);

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

int chttp_server_websocket_with(chttp_server *server,
                                const chttp_server_websocket_options *options);

int chttp_server_websocket_with_jwt_bearer(chttp_server *server,
                                           const chttp_server_websocket_options *options,
                                           chttp_jwt_bearer_validator *validator);

int chttp_server_websocket(chttp_server *server, const char *path, chttp_websocket_open_fn on_open,
                           chttp_websocket_event_fn on_event, void *user);

int chttp_server_use(chttp_server *server, chttp_server_middleware_fn middleware, void *user);

int chttp_server_use_cors(chttp_server *server, const chttp_server_cors_options *options);

int chttp_server_start(chttp_server *server);

int chttp_server_port(const chttp_server *server, uint16_t *out_port);

int chttp_server_stop(chttp_server *server, uint32_t timeout_ms);

int chttp_server_destroy(chttp_server *server);

int chttp_server_get_stats(const chttp_server *server, chttp_server_stats *out_stats);
#ifdef __cplusplus
}
#endif
#endif
