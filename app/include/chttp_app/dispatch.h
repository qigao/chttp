#ifndef CHTTP_APP_DISPATCH_H
#define CHTTP_APP_DISPATCH_H
#include <http_server/http.h>
#include <http_common/method.h>

typedef struct chttp_worker_config {
  /* Positive finite limits. retained_bytes is the total overlapping snapshot/
   * draft/resource reservation budget, not a per-request body limit. One worker
   * serializes business; multiple workers require concurrency-safe Beans. */
  size_t workers, capacity, retained_bytes, reply_bytes;
  /* Extra copied response headers; Content-Type shares the reply arena. */
  size_t header_capacity, header_bytes;
} chttp_worker_config;
typedef struct chttp_worker_stats {
  size_t pending, retained_bytes, session_waiting;
  uint64_t accepted, rejected, completed, cancelled;
  int publication_status;
} chttp_worker_stats;
typedef struct chttp_invocation_result { int status; bool rejected; } chttp_invocation_result;
/* Worker-only terminal observer. Snapshot/outcome expire on return. Never
 * retain their views, close/wait on the App, or throw/longjmp. */
typedef void (*chttp_completion_fn)(void *, const chttp_server_request_view *, const chttp_response_outcome *);
/* Header-only worker scope: no body or mutable Session authority. */
typedef int (*chttp_admission_fn)(void *, const chttp_server_request_view *, chttp_server_admission_result *);
typedef int (*chttp_upload_open_fn)(void *, const chttp_server_request_view *, chttp_body_sink *);
/* Event views expire on return; peer values retain the existing bounded,
 * generation-checked cross-thread command contract. Events are serial per peer.
 * The ready callback has event == NULL; all other callbacks have an event. */
typedef struct chttp_websocket_event_view {
  const chttp_server_request_view *request;
  const chttp_server_websocket_session *peer;
  const chttp_websocket_event *event;
} chttp_websocket_event_view;
typedef int (*chttp_websocket_event_action_fn)(void *, const chttp_websocket_event_view *);
typedef struct chttp_asset_policy_result { bool immutable; const char *etag; } chttp_asset_policy_result;
typedef int (*chttp_asset_policy_fn)(void *, const char *, chttp_asset_policy_result *);
#endif
