#ifndef CHTTP_INVOCATION_INTERNAL_H
#define CHTTP_INVOCATION_INTERNAL_H
#include <chttp_app/invocation.h>
#include "chttp_application_work.h"
#include <threads/threads.h>

typedef struct chttp_invocation_state {
  chttp_server_request_view request;
  chttp_session *session;
  chttp_application_work_reply *reply;
  chttp_invocation_result *before_result;
  const void *thread_token;
} chttp_invocation_state;
/* Internal authorization decision after its denial response has been staged. */
int chttp_invocation_mark_rejected(const chttp_invocation *, bool *);
static inline chttp_invocation_state chttp_invocation_state_make(const chttp_server_request_view *request,
    chttp_application_work_reply *reply, chttp_invocation_result *before_result) {
  chttp_invocation_state state = {*request, request->session, reply, before_result, sync_thread_current_token()};
  /* This is the detached, worker-affine intent view, never the engine store. */
  state.request.session = state.session;
  return state;
}
#endif
