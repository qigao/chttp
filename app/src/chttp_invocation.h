#ifndef CHTTP_INVOCATION_H
#define CHTTP_INVOCATION_H
/* Private buffered Controller migration. Not installed. Only the runtime
 * constructs this borrowed handle; never retain it or its accessor views. */
#include "chttp_application_work.h"
#include <cmeta/function.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_invocation { const void *impl; } chttp_invocation;
typedef int (*chttp_invocation_action_fn)(void *, const chttp_invocation *);
static const cmeta_type_identity chttp_invocation_identity = CMETA_TYPE_ID_ATOM_INIT("chttp.invocation");
static const cmeta_type_desc chttp_invocation_type = {"chttp_invocation", sizeof(chttp_invocation),
    CMETA_ALIGNOF(chttp_invocation), CMETA_T_OBJECT, NULL, NULL, &chttp_invocation_identity};
static const cmeta_type_desc chttp_invocation_pointer_type = {"const chttp_invocation *",
    sizeof(chttp_invocation *), CMETA_ALIGNOF(chttp_invocation *), CMETA_T_POINTER,
    &chttp_invocation_type, NULL, NULL};
#define CHTTP_INVOCATION_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, \
      &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_invocation *, invocation, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
      &chttp_invocation_pointer_type, CMETA_ABI_OBJECT_POINTER))

typedef chttp_application_work_result chttp_invocation_result;
CMETA_INTERCEPTOR_TYPE(chttp_invocation_chain, chttp_invocation, chttp_invocation_result);
/* Hooks follow the SDK ACE no-throw/longjmp contract. Contexts come from the
 * selected canonical Bean, not an arbitrary raw callback receiver. */
typedef struct chttp_invocation_interceptor {
  const char *component_id;
  const cmeta_data_desc *receiver;
  chttp_invocation_chain_before_fn before;
  chttp_invocation_chain_after_fn after;
  chttp_invocation_chain_error_fn on_error;
} chttp_invocation_interceptor;

/* Worker/callback scoped. Invalid or foreign-thread handles return NULL.
 * request has no mutable Session handle; session is the detached intent view. */
const chttp_server_request_view *chttp_invocation_request(const chttp_invocation *);
chttp_session *chttp_invocation_session(const chttp_invocation *);
const cmeta_function_desc *chttp_invocation_operation(const chttp_invocation *);
/* Returns EINVAL on invalid scope/output. On success, reports only an observed
 * failed native terminal. Cancellation is not permission to free native owners. */
int chttp_invocation_cancelled(const chttp_invocation *, bool *);
/* Same bounded copied draft/status/content-type contract as the worker engine.
 * EALREADY after sealing; EMSGSIZE leaves the draft unsealed and reusable. */
int chttp_invocation_respond(const chttp_invocation *, unsigned int, const char *, const void *, size_t);
/* Copied case-insensitive upsert; valid through entered hook unwind, including
 * after respond(). Count/byte exhaustion returns ENOBUFS without mutation;
 * reserved transport fields return EPERM, invalid fields EINVAL. Content-Type
 * uses the reply arena (EMSGSIZE on exhaustion), with one authoritative row. */
int chttp_invocation_set_header(const chttp_invocation *, const char *, const char *);
/* Before-only handled rejection; success seals the draft and clears proceed. */
int chttp_invocation_reject(const chttp_invocation *, bool *, unsigned int, const char *, const void *, size_t);

#ifdef __cplusplus
}
#endif
#endif
