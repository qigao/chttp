#ifndef CHTTP_METHOD_HOOKS_H
#define CHTTP_METHOD_HOOKS_H
/* Private, scheduler-neutral generated method boundary. Not installed. */
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

static inline bool chttp_method_hooks_valid(const chttp_method_chain_hook *hooks, size_t count) {
  if (count > CHTTP_METHOD_MAX_HOOKS || (count && !hooks)) return false;
  for (size_t i = 0u; i < count; ++i)
    if (!hooks[i].before && !hooks[i].after && !hooks[i].on_error) return false;
  return true;
}
/* Before-only explicit authorization rejection; no mutable engine builder. */
static inline cmeta_status chttp_method_call_reject(const chttp_method_call *call, bool *proceed) {
  if (!call || !call->result || !proceed) return CMETA_INVALID_ARGUMENT;
  call->result->rejected = true; *proceed = false;
  return CMETA_OK;
}
/* A successful business chain requires a staged successful native outcome.
 * Native/typed errors retain their distinct wire mapping despite ACE error. */
static inline cmeta_status chttp_method_result_status(const chttp_method_result *result) {
  return result->status == DATA_BIND_OK && !result->rejected &&
      result->outcome.kind == DATA_BIND_BINDING_OUTCOME_SUCCESS ? CMETA_OK : CMETA_CALLBACK_ERROR;
}
#endif
