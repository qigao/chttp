#ifndef CHTTP_METHOD_HOOKS_H
#define CHTTP_METHOD_HOOKS_H
/* Private, scheduler-neutral generated method boundary. Not installed. */
#include <http_common/method.h>
#include <cmeta/ace_interceptor.h>
#include <core/error_codes.h>
#include <stdint.h>
struct chttp_server_request_view;
struct chttp_session;

typedef struct chttp_method_hook {
  void *context;
  chttp_method_chain_before_fn before;
  chttp_method_chain_after_fn after;
  chttp_method_chain_error_fn on_error;
  chttp_method_completion_fn after_completion;
} chttp_method_hook;
typedef struct chttp_method_completion_record {
  const chttp_method_hook *hooks;
  size_t entered;
  chttp_method_terminal terminal;
} chttp_method_completion_record;
/* Request-owned fixed storage, reserved before any method side effect. */
typedef struct chttp_method_completion_sink {
  void *context;
  int (*reserve)(void *, chttp_method_completion_record **);
} chttp_method_completion_sink;

static inline bool chttp_method_hooks_valid(const chttp_method_hook *hooks, size_t count) {
  if (count > CHTTP_METHOD_MAX_HOOKS || (count && !hooks)) return false;
  for (size_t i = 0u; i < count; ++i)
    if (!hooks[i].before && !hooks[i].after && !hooks[i].on_error && !hooks[i].after_completion) return false;
  return true;
}
static inline bool chttp_method_hooks_have_completion(const chttp_method_hook *hooks, size_t count) {
  for (size_t i = 0u; i < count; ++i) if (hooks[i].after_completion) return true;
  return false;
}
static inline int chttp_method_completion_reserve(const chttp_method_completion_sink *sink,
    const chttp_method_hook *hooks, size_t count, chttp_method_completion_record **out) {
  *out = NULL;
  if (!chttp_method_hooks_have_completion(hooks, count)) return SALTS_OK;
  if (!sink || !sink->reserve) return SALTS_EINVAL;
  const int status = sink->reserve(sink->context, out);
  if (status == SALTS_OK) (*out)->hooks = hooks;
  return status;
}
typedef struct chttp_method_hook_proxy {
  const chttp_method_hook *hook;
  chttp_method_completion_record *record;
  size_t index;
} chttp_method_hook_proxy;
static inline cmeta_status chttp_method_hook_before(void *user, const chttp_method_call *call, bool *proceed) {
  chttp_method_hook_proxy *proxy = (chttp_method_hook_proxy *)user;
  if (proxy->record) proxy->record->entered = proxy->index + 1u;
  return proxy->hook->before ? proxy->hook->before(proxy->hook->context, call, proceed) : CMETA_OK;
}
static inline void chttp_method_hook_after(void *user, const chttp_method_call *call, const chttp_method_result *result) {
  const chttp_method_hook *hook = ((chttp_method_hook_proxy *)user)->hook;
  if (hook->after) hook->after(hook->context, call, result);
}
static inline void chttp_method_hook_error(void *user, const chttp_method_call *call, cmeta_status status) {
  const chttp_method_hook *hook = ((chttp_method_hook_proxy *)user)->hook;
  if (hook->on_error) hook->on_error(hook->context, call, status);
}
static inline cmeta_status chttp_method_invoke(void *context, chttp_method_chain_target_fn target,
    const chttp_method_hook *hooks, size_t count, const chttp_method_call *call, chttp_method_result *result,
    chttp_method_completion_record *record) {
  if (!target || !call || !result || !chttp_method_hooks_valid(hooks, count)) return CMETA_INVALID_ARGUMENT;
  chttp_method_hook_proxy proxies[CHTTP_METHOD_MAX_HOOKS];
  chttp_method_chain_hook entered[CHTTP_METHOD_MAX_HOOKS];
  for (size_t i = 0u; i < count; ++i) {
    const chttp_method_hook_proxy proxy = {&hooks[i], record, i};
    proxies[i] = proxy;
    const chttp_method_chain_hook hook = {&proxies[i], chttp_method_hook_before,
        chttp_method_hook_after, chttp_method_hook_error};
    entered[i] = hook;
  }
  const chttp_method_chain chain = {context, target, entered, count};
  const cmeta_status status = chttp_method_chain_invoke(&chain, call, result);
  const chttp_method_terminal terminal = {call->operation, call->rpc_method, call->rpc_id,
      call->notification, result->status, result->native_status, result->invoked, result->rejected,
      result->outcome, status, SALTS_EPROTO};
  if (record) record->terminal = terminal;
  return status;
}
static inline int chttp_method_response_status(chttp_method_completion_record *record, int status) {
  if (record) record->terminal.response_status = status;
  return status;
}
/* A successful business chain requires a staged successful native outcome.
 * Native/typed errors retain their distinct wire mapping despite ACE error. */
static inline cmeta_status chttp_method_result_status(const chttp_method_result *result) {
  return result->status == DATA_BIND_OK && !result->rejected &&
      result->outcome.kind == DATA_BIND_BINDING_OUTCOME_SUCCESS ? CMETA_OK : CMETA_CALLBACK_ERROR;
}
#endif
