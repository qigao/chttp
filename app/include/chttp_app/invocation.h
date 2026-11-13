#ifndef CHTTP_INVOCATION_H
#define CHTTP_INVOCATION_H
/* Worker-scoped Controller invocation. Only the runtime constructs this
 * borrowed handle; never retain it or its accessor views. */
#include <chttp_app/dispatch.h>
#include <chttp_app/web.h>
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
#define CHTTP_ACTION(name_) \
  FunctionDeclAsAbiResult(io, int, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, name_, \
    (void *, self, CMETA_PARAM_INOUT | CMETA_PARAM_BORROWED | CMETA_PARAM_RECEIVER, \
      &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER), \
    (const chttp_invocation *, invocation, CMETA_PARAM_IN | CMETA_PARAM_BORROWED, \
      &chttp_invocation_pointer_type, CMETA_ABI_OBJECT_POINTER))

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
 * request->session and session() expose the same detached intent view;
 * neither exposes a live engine Session or may escape this callback. */
const chttp_server_request_view *chttp_invocation_request(const chttp_invocation *);
chttp_session *chttp_invocation_session(const chttp_invocation *);
const cmeta_function_desc *chttp_invocation_operation(const chttp_invocation *);
/* Returns EINVAL on invalid scope/output. On success, reports only an observed
 * failed native terminal. Cancellation is not permission to free native owners. */
int chttp_invocation_cancelled(const chttp_invocation *, bool *);
/* Same bounded copied draft/status/content-type contract as the worker engine.
 * EALREADY after sealing; EMSGSIZE leaves the draft unsealed and reusable. */
int chttp_invocation_respond(const chttp_invocation *, unsigned int, const char *, const void *, size_t);
/* Opening-only explicit acceptance. Normal buffered respond() rejects the
 * upgrade. Session intents and entered hooks settle before native publication. */
int chttp_invocation_websocket_accept(const chttp_invocation *);
/* Copied case-insensitive upsert; valid through entered hook unwind, including
 * after respond(). Count/byte exhaustion returns ENOBUFS without mutation;
 * reserved transport fields return EPERM, invalid fields EINVAL. Content-Type
 * uses the reply arena (EMSGSIZE on exhaustion), with one authoritative row. */
int chttp_invocation_set_header(const chttp_invocation *, const char *, const char *);
/* Copied raw-file decision, charged to reply_bytes. Body-bearing 200..599
 * statuses only (not 204/205/304); HEAD is handled by the native engine.
 * EINVAL/EMSGSIZE/ERANGE leaves the draft unsealed; EALREADY after selection.
 * NULL type preserves Content-Type. No filesystem operation occurs here:
 * owner preparation errors use after_completion.publication_status, while
 * subsequent read/wire failures use the native transport outcome.
 * The file must remain immutable through the native terminal. No URL mapping
 * or conditional/range policy is added. Headers follow normal hook ordering. */
int chttp_invocation_response_file(const chttp_invocation *, unsigned int,
    const char *, const char *);
/* serve_file counterpart using the owned request snapshot. Metadata
 * and conditional/range policy run on the worker; path/MIME and response fields
 * are copied into reply/header budgets. EINVAL/ENOBUFS/EMSGSIZE and filesystem
 * errors leave the draft unchanged; EALREADY after response selection. Missing
 * metadata is a business error; subsequent native open/read outcomes follow
 * response_file. The selected file must stay immutable through native terminal.
 * Headers retain before/action/after ordering. No URL mapping is performed. */
int chttp_invocation_serve_file(const chttp_invocation *, const chttp_server_file_options *);
/* Copies the synchronous source descriptor and reserves chunk_bytes in the
 * existing reply arena. Success transfers user-resource lifetime through the
 * worker cleanup callback; failure transfers nothing. Reads/cleanup are serial
 * worker callbacks after action/hook unwind, with zero bytes meaning EOF.
 * No native builder or Invocation scope may be retained by the source. */
int chttp_invocation_response_source(const chttp_invocation *, unsigned int, const char *,
    const chttp_body_source *, void (*cleanup)(void *, int), size_t chunk_bytes);
/* SSE formatting/next/close reuse the web helper on workers. Scratch and user
 * borrow the same successful source-selection lifetime through cleanup. */
struct chttp_web_sse_stream;
int chttp_invocation_sse_response(const chttp_invocation *, struct chttp_web_sse_stream *, size_t chunk_bytes);
/* Validates CSRF and commits the upload on this worker using the detached
 * Session view. The request must belong to this upload's copied sink user. */
chttp_web_status chttp_invocation_upload_finalize(const chttp_invocation *, chttp_web_upload_request *,
    const chttp_web_validation *, chttp_web_error *);
/* Before-only handled rejection; success seals the draft and clears proceed. */
int chttp_invocation_reject(const chttp_invocation *, bool *, unsigned int, const char *, const void *, size_t);

#ifdef __cplusplus
}
#endif
#endif
