#ifndef CHTTP_SERVICE_SERVICE_H
#define CHTTP_SERVICE_SERVICE_H

#include <http_server/http.h>
#include <http_server/rpc.h>

#include <data_bind_method_plan.h>
#include <data_bind_native.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_service {
  void *impl;
} chttp_service;

/**
 * Bounded synchronous MethodPlan runtime.
 *
 * The first slice intentionally admits scalar/text HTTP bindings only. Native
 * workspace is shared and the service is therefore pinned to the first
 * underlying CHttp::Server used for a successful mount. Additional mounts must
 * use that same owner thread; a different server returns SALTS_EBUSY.
 * Deferred execution is a later slice.
 */
typedef struct chttp_service_config {
  size_t size;
  size_t method_capacity;
  size_t max_binding_value_bytes;
  size_t max_response_body_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
} chttp_service_config;

#define CHTTP_SERVICE_CONFIG_INIT \
  { sizeof(chttp_service_config), 0u, 1024u, 4096u, 16384u, 16u, 256u, 65536u }

/**
 * Exact-ABI operation callback.
 *
 * The CHttp transport adapter owns transport extraction/publication. The
 * callback owns only exact native staging and invocation:
 *
 *   BindingPlan bind_inputs -> exact C call -> BindingPlan write_outcome
 *
 * It must not inspect/compile HTTP projection metadata and must not retain any
 * borrowed argument after return.
 *
 * Return SALTS_EINVAL only for caller/input rejection. The mounted transport
 * maps that class to its protocol-level invalid-input response. Other negative
 * Salts errors are treated as server/runtime failures.
 */
typedef int (*chttp_service_invoke_fn)(
    void *user,
    const DataBindBindingPlan *binding,
    const DataBindBindingProvider *provider,
    const DataBindNativeOptions *native_options,
    DataBindBindingOutcome *outcome,
    DataBindBindingPlanDiagnostic *diagnostic);

typedef struct chttp_service_http_mount {
  size_t size;
  /** Borrowed immutable plan; must outlive the mounted service/server route. */
  const DataBindHttpMethodPlan *plan;
  chttp_service_invoke_fn invoke;
  void *user;
  const chttp_server_middleware *middleware;
  size_t middleware_count;
} chttp_service_http_mount;

#define CHTTP_SERVICE_HTTP_MOUNT_INIT \
  { sizeof(chttp_service_http_mount), NULL, NULL, NULL, NULL, 0u }

typedef struct chttp_service_rpc_mount {
  size_t size;
  /** Fixed CRPC endpoint, e.g. "/rpc". Deployment/runtime concern. */
  const char *target;
  /** Borrowed immutable plan; must outlive the mounted service/server method. */
  const DataBindRpcMethodPlan *plan;
  chttp_service_invoke_fn invoke;
  void *user;
} chttp_service_rpc_mount;

#define CHTTP_SERVICE_RPC_MOUNT_INIT \
  { sizeof(chttp_service_rpc_mount), NULL, NULL, NULL, NULL }

/** Initialize an empty bounded registry. */
int chttp_service_init(
    chttp_service *service, const chttp_service_config *config);

/**
 * Mount one already-compiled HTTP MethodPlan onto the existing CHttp router.
 *
 * No DataBind schema, projection config or CMeta reflection is interpreted by
 * this function. The MethodPlan method/route are the transport authority.
 */
int chttp_service_mount_http(
    chttp_service *service,
    chttp_server *server,
    const chttp_service_http_mount *mount);

/**
 * Register one already-compiled RPC MethodPlan on an existing CRPC server.
 *
 * The RPC wire method comes only from the MethodPlan. target remains a runtime
 * endpoint/deployment value owned by CRPC, not canonical DataBind IDL.
 *
 * Phase 1 admits scalar named/positional params and at most one scalar result.
 * Structured params/results fail closed pending FormatPlan integration.
 */
int chttp_service_mount_rpc(
    chttp_service *service,
    crpc_server *server,
    const chttp_service_rpc_mount *mount);

/**
 * Release owned route strings/scratch.
 *
 * Servers containing routes mounted by this service must be stopped and
 * destroyed first. Borrowed MethodPlans are never freed here.
 */
int chttp_service_destroy(chttp_service *service);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_SERVICE_H */
