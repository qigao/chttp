#ifndef CHTTP_APPLICATION_WORK_H
#define CHTTP_APPLICATION_WORK_H
/* Private migration bridge. Not an installed Application execution mode.
 * Buffered requests only; Session state is a detached, staged snapshot. The public model switches after
 * Session/stream/WS and full response-terminal adapters pass their own tests. */
#include <http_server/http.h>
#include <cmeta/ace_interceptor.h>
#include "../../http_common/http/chttp_method_hooks.h"

typedef struct chttp_application_work { void *impl; } chttp_application_work;
typedef struct chttp_application_work_reply chttp_application_work_reply;
typedef struct chttp_application_work_result { int status; bool rejected; } chttp_application_work_result;
typedef struct chttp_application_work_exchange {
  const chttp_server_request_view *request;
  chttp_application_work_reply *response;
  chttp_application_work_result *result;
} chttp_application_work_exchange;
CMETA_INTERCEPTOR_TYPE(chttp_application_work_chain,
    chttp_application_work_exchange, chttp_application_work_result);
enum { CHTTP_APPLICATION_WORK_MAX_HOOKS = 16 };
typedef int (*chttp_application_work_fn)(void *, const chttp_server_request_view *,
    chttp_application_work_reply *);
typedef chttp_response_outcome chttp_application_work_outcome;
typedef void (*chttp_application_work_completion_fn)(void *,
    const chttp_server_request_view *, const chttp_application_work_outcome *);
typedef struct chttp_application_work_task {
  chttp_application_work_fn run;
  void (*finalize)(void *);
  void *user;
  /* Descriptors are copied on acceptance; hook contexts borrow the same
   * Application/Bean lifetime as user through task finalization. */
  const chttp_application_work_chain_hook *hooks;
  size_t hook_count;
  chttp_application_work_completion_fn after_completion;
  /* Request-owned method records, charged to retained_bytes before defer. */
  size_t method_capacity;
} chttp_application_work_task;
typedef struct chttp_application_work_config {
  size_t workers;
  size_t capacity;
  size_t retained_bytes;
  size_t reply_bytes;
  /* Extra fields only. Both zero select no extras; Content-Type shares the
   * reply arena and its reserved metadata row. All bytes count at admission. */
  size_t header_capacity;
  size_t header_bytes;
} chttp_application_work_config;
typedef struct chttp_application_work_stats {
  size_t pending;
  size_t retained_bytes;
  size_t session_waiting;
  /* Lifetime telemetry saturates; capacity/byte accounting never saturates. */
  uint64_t accepted;
  uint64_t rejected;
  uint64_t completed;
  uint64_t cancelled;
  int publication_status;
} chttp_application_work_stats;

#ifdef __cplusplus
extern "C" {
#endif
int chttp_application_work_init(chttp_application_work *, const chttp_application_work_config *);
/* Transactional until response_defer succeeds. Rejection invokes no callback.
 * Accepted work owns its snapshot and borrows task.user through finalize.
 * Metadata (operation) remains borrowed from the live Application graph.
 * after_completion and finalize run on workers only, after business settlement
 * joins the real owner response terminal (success/reset/close). */
int chttp_application_work_submit(chttp_application_work *, const chttp_server_request_view *,
    chttp_server_response *, const chttp_application_work_task *);
/* Owner-side conversion of rejected admission after caller-owned payload
 * cleanup: overload/closed -> 503; stale Session generation -> 409. */
int chttp_application_work_admission_response(chttp_server_response *, int);
/* Worker-only copied response draft. No engine builder crosses the queue.
 * A NULL content type preserves the engine's header-free empty reply contract. */
int chttp_application_work_respond(chttp_application_work_reply *, unsigned int,
    const char *, const void *, size_t);
int chttp_application_work_set_header(chttp_application_work_reply *, const char *, const char *);
int chttp_application_work_reject(const chttp_application_work_exchange *, unsigned int,
    const char *, const void *, size_t);
int chttp_application_work_reply_current(const chttp_application_work_reply *);
int chttp_application_work_cancelled(const chttp_application_work_reply *, bool *);
chttp_method_completion_sink chttp_application_work_method_sink(chttp_application_work_reply *);
int chttp_application_work_is_current(const chttp_application_work *);
int chttp_application_work_get_stats(chttp_application_work *, chttp_application_work_stats *);
/* A known publication failure ends a pending wait immediately; live records
 * remain owned. A successful wait only establishes quiescence, not wire success. */
int chttp_application_work_wait(chttp_application_work *, uint32_t timeout_ms);
/* Stop admission, drain accepted work, then destroy the owned worker pool.
 * Finite timeout and same-executor close retain ownership on failure.
 * Destroy is control-plane only: caller first quiesces submitters/observers,
 * including owner callbacks that can enter submit, before successful close. */
int chttp_application_work_close(chttp_application_work *, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
