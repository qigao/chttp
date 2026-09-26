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
  size_t max_output_value_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_rpc_service_config;

#define CHTTP_RPC_SERVICE_CONFIG_INIT \
  { sizeof(chttp_rpc_service_config), 0u, 4096u, 16384u, 16u, 256u, 65536u }

/**
 * Exact native RPC operation adapter.
 *
 * The callback owns exact staging only. RPC wire identity and field/error
 * projection are already compiled into method_plan.
 *
 * A typical adapter:
 *   1. creates exact request/param/return/error staging;
 *   2. calls data_bind_binding_plan_bind_inputs();
 *   3. invokes the exact C signature;
 *   4. calls data_bind_binding_plan_write_outcome();
 *   5. returns the canonical DataBindBindingOutcome.
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
  /** Fixed CRPC HTTP endpoint, for example "/rpc". Borrowed until register returns. */
  const char *target;
  const DataBindRpcMethodPlan *method_plan;
  chttp_rpc_service_exact_fn invoke;
  void *user;
} chttp_rpc_service_mount;

#define CHTTP_RPC_SERVICE_MOUNT_INIT \
  { sizeof(chttp_rpc_service_mount), NULL, NULL, NULL, NULL }

/**
 * Initialize bounded owner-thread staging for generated RPC MethodPlan mounts.
 *
 * Phase 1 supports independent name/ordinal params selection and one scalar
 * result/typed-error data value. Structured output remains fail-closed until
 * shared DataBind FormatPlan support is mounted.
 */
int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config);

/**
 * Register one already-compiled DataBind RPC MethodPlan on CRPC.
 *
 * The MethodPlan and native descriptors referenced by its BindingPlan are
 * borrowed until chttp_rpc_service_destroy().
 */
int chttp_rpc_service_mount(
    chttp_rpc_service *service,
    crpc_server *server,
    const chttp_rpc_service_mount *mount);

/**
 * Release service-owned method records and bounded staging.
 *
 * The CRPC server must be stopped and destroyed before this call.
 */
int chttp_rpc_service_destroy(chttp_rpc_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_RPC_SERVICE_SERVICE_H */
