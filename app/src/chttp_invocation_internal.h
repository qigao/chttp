#ifndef CHTTP_INVOCATION_INTERNAL_H
#define CHTTP_INVOCATION_INTERNAL_H
#include "chttp_invocation.h"
#include <threads/threads.h>

typedef struct chttp_invocation_state {
  chttp_server_request_view request;
  chttp_session *session;
  chttp_application_work_reply *reply;
  chttp_invocation_result *before_result;
  const void *thread_token;
} chttp_invocation_state;
static inline chttp_invocation_state chttp_invocation_state_make(const chttp_server_request_view *request,
    chttp_application_work_reply *reply, chttp_invocation_result *before_result) {
  chttp_invocation_state state = {*request, request->session, reply, before_result, sync_thread_current_token()};
  state.request.session = NULL;
  return state;
}
#endif
