#ifndef CHTTP_RPC_SERVICE_SERVICE_H
#define CHTTP_RPC_SERVICE_SERVICE_H

#include <http_server/rpc.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_rpc_service {
  void *impl;
} chttp_rpc_service;

typedef struct chttp_rpc_service_config {
  size_t size;
  size_t method_capacity;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_rpc_service_config;

#define CHTTP_RPC_SERVICE_CONFIG_INIT \
  { sizeof(chttp_rpc_service_config), 0u, 16384u, 16u, 256u, 65536u }

/**
 * Exact native operation adapter for one generated RPC MethodPlan.
 *
 * The callback owns exact request/parameter/return/error staging and normally:
 *   1. binds inputs through data_bind_binding_plan_bind_inputs();
 *   2. invokes the exact C signature;
 *   3. publishes via data_bind_binding_plan_write_outcome().
 *
 * RpcService owns only JSON-RPC/BindingPlan provider glue. It performs no
 * FunctionDesc lookup or dynamic/universal invocation.
 */
typedef DataBindStatus (*chttp_rpc_service_exact_fn)(
    void *user,
    const DataBindBindingPlan *plan,
    const DataBindBindingProvider *provider,
    const DataBindNativeOptions *native_options,
    DataBindBindingOutcome *outcome,
    DataBindBindingPlanDiagnostic *diagnostic);

typedef struct chttp_rpc_service_mount {
  size_t size;
  const char *target;
  const DataBindRpcMethodPlan *method_plan;
  chttp_rpc_service_exact_fn invoke;
  void *user;
} chttp_rpc_service_mount;

#define CHTTP_RPC_SERVICE_MOUNT_INIT \
  { sizeof(chttp_rpc_service_mount), NULL, NULL, NULL, NULL }

int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config);

/**
 * Register one already-compiled DataBind RPC MethodPlan on a stopped CRPC
 * server. The MethodPlan/native descriptors are borrowed until destroy().
 */
int chttp_rpc_service_mount(
    chttp_rpc_service *service,
    crpc_server *server,
    const chttp_rpc_service_mount *mount);

/** Release service-owned method records/workspace after the CRPC server stops. */
int chttp_rpc_service_destroy(chttp_rpc_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_RPC_SERVICE_SERVICE_H */
