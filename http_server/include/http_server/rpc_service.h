#ifndef HTTP_SERVER_RPC_SERVICE_H
#define HTTP_SERVER_RPC_SERVICE_H

#include <http_server/rpc.h>
#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>

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
} chttp_server_rpc_mount_options;

#define CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT \
  { sizeof(chttp_server_rpc_mount_options), NULL, NULL, NULL, NULL }

/**
 * Admit one generated method into a configured chttp_server RPC registry.
 * Function ABI, binding and ownership are verified before publishing the
 * wire method. Each concurrent call leases its own bounded native frame;
 * no mutable request/response/workspace is stored in the method record.
 * All release is owned by chttp_server_destroy(), after completed drain.
 */
int chttp_server_rpc_mount(chttp_server *server,
                           const chttp_server_rpc_mount_options *mount);

#ifdef __cplusplus
}
#endif
#endif /* HTTP_SERVER_RPC_SERVICE_H */
