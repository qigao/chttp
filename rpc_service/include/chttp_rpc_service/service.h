#ifndef CHTTP_RPC_SERVICE_SERVICE_H
#define CHTTP_RPC_SERVICE_SERVICE_H

#include <http_server/rpc.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>
#include <data_bind_native_binding.h>

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
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_rpc_service_config;

#define CHTTP_RPC_SERVICE_CONFIG_INIT \
  { sizeof(chttp_rpc_service_config), 0u, 4096u, 65536u, 16384u, 16u, 256u, 65536u }

/**
 * One generated RPC operation mount.
 *
 * Native semantic identity is producer-owned:
 * - method_plan owns RPC wire/binding/error projection;
 * - native_binding owns request/response/error storage layout;
 * - execution owns FunctionDesc/FunctionAbi + generated exact adapter.
 *
 * Mount validates these artifacts before registering the CRPC method.
 */
typedef struct chttp_rpc_service_mount {
  size_t size;
  /** Fixed CRPC HTTP endpoint, for example "/rpc". Borrowed until register returns. */
  const char *target;
  const DataBindRpcMethodPlan *method_plan;
  const DataBindServiceNativeBinding *native_binding;
  const DataBindNativeExecution *execution;
} chttp_rpc_service_mount;

#define CHTTP_RPC_SERVICE_MOUNT_INIT \
  { sizeof(chttp_rpc_service_mount), NULL, NULL, NULL, NULL }

/**
 * Initialize bounded owner-thread execution storage for generated RPC MethodPlans.
 *
 * Current slice supports independent name/ordinal params selection and one
 * scalar/string/bytes result or typed-error data value. Structured output
 * remains fail-closed until shared producer-owned composition is qualified.
 */
int chttp_rpc_service_init(
    chttp_rpc_service *service,
    const chttp_rpc_service_config *config);

/**
 * Register one already-compiled DataBind RPC MethodPlan on CRPC.
 *
 * method_plan, native_binding, execution, and all referenced descriptors are
 * borrowed until chttp_rpc_service_destroy(). Function/ABI/layout admission is
 * performed once before CRPC method registration; the call hot path performs no
 * reflection lookup or dynamic ABI reconstruction.
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
