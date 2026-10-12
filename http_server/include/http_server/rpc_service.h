#ifndef HTTP_SERVER_RPC_SERVICE_H
#define HTTP_SERVER_RPC_SERVICE_H

#include <http_server/rpc.h>
#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>
#include <salts/plugin.h>

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
} chttp_server_rpc_mount_options;

#define CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT \
  { sizeof(chttp_server_rpc_mount_options), NULL, NULL, NULL, NULL, NULL, {0} }

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

#ifdef __cplusplus
}
#endif
#endif /* HTTP_SERVER_RPC_SERVICE_H */
