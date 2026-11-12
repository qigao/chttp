#ifndef CHTTP_RPC_MOUNT_H
#define CHTTP_RPC_MOUNT_H

#include <http_server/rpc.h>
#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>
#include <cmeta/plugin.h>
#include "../../http_common/http/chttp_method_hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One immutable generated DataBind RPC operation projected onto the already
 * initialized HTTP server. All producer descriptors remain borrowed until
 * the server successfully stops/drains and is destroyed.
 */
typedef struct chttp_server_rpc_mount_options {
  size_t size;
  /** Fixed origin-form JSON-RPC path, borrowed during mount. */
  const char *target;
  const DataBindRpcMethodPlan *method_plan;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;
  /** Optional existing Plugin generation pin; both fields must be supplied.
   * Registry remains caller-owned and must outlive Server drain/destroy.
   * Does not by itself prove that the execution was exported by this DSO. */
  cmeta_plugin_registry *plugin_registry;
  cmeta_plugin_ref plugin_ref;
  const chttp_method_hook *hooks;
  size_t hook_count;
  DataBind *output_codec; /* Private worker composition; mount-time borrow. */
} chttp_server_rpc_mount_options;

#define CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT \
  { sizeof(chttp_server_rpc_mount_options), NULL, NULL, NULL, NULL, NULL, {0}, NULL, 0u, NULL }

/**
 * Admit one generated method into a configured chttp_server RPC registry.
 * Function ABI, binding and ownership are verified before publishing the
 * wire method. Each concurrent call leases its own bounded native frame;
 * no mutable request/response/workspace is stored in the method record.
 * All release is owned by chttp_server_destroy(), after completed drain.
 * When plugin_registry/plugin_ref are provided, a registry lease pins the
 * named Plugin generation from mount through native terminal drain/destroy.
 * A mismatched or stopped generation fails admission without publication.
 */
int chttp_server_rpc_mount(chttp_server *server,
                           const chttp_server_rpc_mount_options *mount);

/* Private Application composition boundary. Set the owner-side dispatch adapter
 * before mounting any methods/listener start. The worker evaluator borrows the
 * immutable registry and per-call bounded frame pool through return; its writer
 * must copy/consume the result synchronously. Server destruction requires the
 * Application's accepted workers to be drained first. No scheduler dependency
 * or Application type belongs to the Server module. */
typedef int (*chttp_rpc_output_fn)(void *, unsigned int, const char *, const void *, size_t);
int chttp_server_rpc_set_dispatch_adapter(chttp_server *, chttp_server_handler_fn, void *);
int chttp_server_rpc_evaluate(chttp_server *, const chttp_server_request_view *,
    chttp_rpc_output_fn, void *, const chttp_method_completion_sink *);
/* Callback-scoped request-owned sink; never retained in the registry. */
const chttp_method_completion_sink *chttp_server_rpc_response_completion_sink(const crpc_server_response *);

#ifdef __cplusplus
}
#endif
#endif /* HTTP_SERVER_RPC_SERVICE_H */
