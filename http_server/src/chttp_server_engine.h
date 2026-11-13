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

/* Move-only private pre-body gate, independent of response-defer ownership.
 * Accepted begin moves/clears gate; rejection retains it and runs no callback.
 * Complete is the sole producer's last native access. Closed pending requests
 * retain the native slot until completion, including finite stop. */
typedef struct chttp_server_admission_gate { void *impl; uint32_t generation; } chttp_server_admission_gate;
typedef int (*chttp_server_admission_begin_fn)(void *, const chttp_server_request_view *,
    chttp_server_admission_gate *);
int chttp_server_set_admission_async(chttp_server *, chttp_server_admission_begin_fn, void *);
int chttp_server_admission_gate_complete(chttp_server_admission_gate *, int,
    const chttp_server_admission_result *);
int chttp_server_admission_gate_cancelled(const chttp_server_admission_gate *, bool *);

/* Private detached Session transaction. Storage is caller-owned, malloc-aligned,
 * charged to its invocation and retained through owner terminal + worker drain.
 * The store/record remain Server-owned. The commit callback only records/signals;
 * it runs on the native owner and may release the logical Session gate. */
typedef struct chttp_session_work chttp_session_work;
int chttp_session_work_size(const chttp_session *, size_t *);
int chttp_session_work_prepare(void *, size_t, const chttp_session *,
    void (*)(void *, int), void *, chttp_session_work **);
chttp_session *chttp_session_work_view(chttp_session_work *);
const void *chttp_session_work_key(const chttp_session_work *);
int chttp_session_work_begin(chttp_session_work *);
void chttp_session_work_seal(chttp_session_work *, bool commit);
void chttp_session_work_release(chttp_session_work *);
int chttp_server_response_defer_worker(chttp_server_response *, chttp_server_deferred *,
    chttp_session_work *);

/* Private file decision. Path borrows the retained Invocation through the real
 * native terminal. prepared runs on the owner only if preparation is attempted;
 * it records status, never application work. No native resource crosses threads. */
typedef struct chttp_server_deferred_file {
  const char *path;
  unsigned int status_code;
  void (*prepared)(void *, int);
  void *user;
  uint64_t offset;
  size_t length;
  bool bounded;
} chttp_server_deferred_file;
int chttp_server_deferred_response_file(chttp_server_deferred *, const chttp_header *, size_t,
    const chttp_server_deferred_file *);

/* Private retained worker source. Only this adapter interprets the positive WAIT;
 * public synchronous sources retain their negative-error/zero-EOF contract.
 * ready/read/cleanup are framework bookkeeping, never application callbacks.
 * Descriptor/user borrow the Invocation through native terminal. */
typedef struct chttp_server_worker_source {
  chttp_body_source source;
  bool (*ready)(void *);
  void (*cleanup)(void *, int);
} chttp_server_worker_source;
enum { CHTTP_SERVER_WORKER_SOURCE_WAIT = 1 };
typedef struct chttp_server_owner_waker { void *impl; } chttp_server_owner_waker;
int chttp_server_response_waker(chttp_server_response *, chttp_server_owner_waker *);
int chttp_server_owner_waker_signal(const chttp_server_owner_waker *);
int chttp_server_deferred_response_source(chttp_server_deferred *, unsigned int,
    const chttp_header *, size_t, const chttp_server_worker_source *);
int chttp_server_deferred_websocket_accept(chttp_server_deferred *, const chttp_header *, size_t);
/* Native-owned producer binding. These functions only copy/signal bookkeeping.
 * retire is the last native use, after clearing the binding and CNet quiescence;
 * it is independent of the response/peer transport terminal. */
typedef struct chttp_server_worker_websocket {
  int (*event)(void *, const chttp_websocket_event *);
  bool (*pending)(void *);
  int (*status)(void *);
  void (*opened)(void *);
  void (*retire)(void *);
  void *user;
} chttp_server_worker_websocket;
int chttp_server_websocket_worker_capture(chttp_websocket *, chttp_server_websocket_session *, size_t *);
int chttp_server_websocket_worker_bind(chttp_websocket *, const chttp_server_worker_websocket *);

enum { CHTTP_SERVER_DATE_BYTES = 29, CHTTP_SERVER_DATE_CAPACITY = CHTTP_SERVER_DATE_BYTES + 1,
       CHTTP_SERVER_FILE_ETAG_CAPACITY = 80, CHTTP_SERVER_FILE_RANGE_CAPACITY = 96,
       CHTTP_SERVER_FILE_HEADER_CAPACITY = 4 };
/* Shared value-only file policy. Generated strings live in this address-stable
 * plan; other strings borrow options. Consume/copy before either expires, never
 * copy the plan by value. No file handle or native response crosses threads. */
typedef struct chttp_server_file_plan {
  unsigned int status_code;
  const char *content_type;
  uint64_t offset;
  size_t length;
  bool send_file;
  chttp_header headers[CHTTP_SERVER_FILE_HEADER_CAPACITY];
  size_t header_count;
  char etag[CHTTP_SERVER_FILE_ETAG_CAPACITY];
  char modified[CHTTP_SERVER_DATE_CAPACITY];
  char range[CHTTP_SERVER_FILE_RANGE_CAPACITY];
} chttp_server_file_plan;
int chttp_server_file_plan_make(const chttp_server_request_view *,
    const chttp_server_file_options *, chttp_server_file_plan *);

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
  /* Framework-only asynchronous sink, mutually exclusive with body_open.
   * poll/close are bookkeeping; user callbacks never run on this owner. */
  int (*worker_body_open)(void *, const chttp_server_request_view *, chttp_server_response *, chttp_body_sink *);
  int (*worker_body_poll)(void *);
  size_t worker_body_chunk_bytes;
} chttp_server_route_options;
enum { CHTTP_SERVER_WORKER_BODY_WAIT = 1 };
int chttp_server_worker_body_chunk_bound(const chttp_server *, size_t *);

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
  bool worker;
  const cmeta_function_desc *operation;
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
