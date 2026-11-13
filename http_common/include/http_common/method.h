#ifndef CHTTP_COMMON_METHOD_H
#define CHTTP_COMMON_METHOD_H
/* Scheduler-neutral generated method facts. Views borrow their callback scope. */
#include <data_bind_binding_plan.h>
#include <cmeta/ace_interceptor.h>
#include <stdint.h>
struct chttp_server_request_view;
struct chttp_session;
enum { CHTTP_METHOD_MAX_HOOKS = 16 };
typedef struct chttp_method_result {
  DataBindStatus status;
  int native_status;
  bool invoked, rejected;
  DataBindBindingOutcome outcome;
  /* Borrowed staged output, possibly different from input for a CFlow Plan. */
  const DataBindBindingCallFrame *output;
} chttp_method_result;
#define CHTTP_METHOD_RESULT_INIT \
  {DATA_BIND_OK, 0, false, false, DATA_BIND_BINDING_OUTCOME_INIT, NULL}

typedef struct chttp_method_call {
  const struct chttp_server_request_view *http;
  /* Read-only method-scope authority; http->session is deliberately NULL.
   * Session mutation belongs to the enclosing request transaction. */
  const struct chttp_session *session;
  const cmeta_function_desc *operation;
  const DataBindBindingCallFrame *input;
  /* NULL for HTTP methods; RPC identity is callback-scoped, including notify. */
  const char *rpc_method;
  uint64_t rpc_id;
  bool notification;
  chttp_method_result *result;
} chttp_method_call;
CMETA_INTERCEPTOR_TYPE(chttp_method_chain, chttp_method_call, chttp_method_result);

/* Frozen facts only: no DTO/frame/parser pointer survives business unwind. */
typedef struct chttp_method_terminal {
  const cmeta_function_desc *operation;
  const char *rpc_method;
  uint64_t rpc_id;
  bool notification;
  DataBindStatus status;
  int native_status;
  bool invoked, rejected;
  DataBindBindingOutcome outcome;
  cmeta_status interceptor_status;
  int response_status; /* HTTP draft / RPC envelope adapter, not a wire code. */
} chttp_method_terminal;
typedef struct chttp_response_outcome {
  int business_status, publication_status, transport_status, session_status;
  bool rejected;
} chttp_response_outcome;
typedef struct chttp_method_completion {
  const struct chttp_server_request_view *http;
  const struct chttp_session *session;
  chttp_method_terminal method;
  chttp_response_outcome response;
} chttp_method_completion;
/* Worker-only borrowed view after the joined terminal. Do not retain views,
 * mutate a response/Session, close/wait on the App, or throw/longjmp. */
typedef void (*chttp_method_completion_fn)(void *, const chttp_method_completion *);
/* Before-only explicit authorization rejection; no mutable engine builder. */
static inline cmeta_status chttp_method_call_reject(const chttp_method_call *call, bool *proceed) {
  if (!call || !call->result || !proceed) return CMETA_INVALID_ARGUMENT;
  call->result->rejected = true; *proceed = false;
  return CMETA_OK;
}

#endif
