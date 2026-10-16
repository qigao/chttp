#ifndef CHTTP_SERVICE_HOST_CANDIDATE_H
#define CHTTP_SERVICE_HOST_CANDIDATE_H

/* Private opt-in conformance API; no installed CHttp public ABI change. */
#include <chttp_app/service.h>
#include <salts/service_host.h>
#include "chttp_service_host_component.h"

#ifdef __cplusplus
extern "C" {
#endif
int chttp_service_mount_host_candidate(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    salts_service_host_runtime *runtime, const char *component_id);

/* Branch-only stable-route candidate.
 * The caller must own the ServiceHost runtime, Provider generations and
 * executor until CHttp Service and HTTP Server have stopped/drained. Register
 * exactly one route before server start; prepare a BUILT generation's typed
 * MethodPlan adapter BEFORE publishing it into ServiceHost.
 *
 * Every HTTP request acquires its own nonmoving Scope at admission, selects
 * one of two immutable pre-admitted operation slots by exact generation ID,
 * and holds the Scope through DataBind value cleanup/deferred finalization.
 * This API neither publishes generations nor loads/unloads Provider modules.
 */
int chttp_service_mount_host_switch_candidate(
    chttp_service *service, chttp_server *server,
    const chttp_service_http_mount *mount,
    salts_service_host_runtime *runtime, const char *component_id);

int chttp_service_host_switch_prepare_candidate(
    chttp_service *service, salts_service_host_generation *generation);

/* Explicit, control-thread-only recycling after ServiceHost reports DRAINED.
 * Rejects BUSY while old Scopes or deferred finalizers remain. No automatic
 * N+2 fallback or third attached generation is ever admitted. */
int chttp_service_host_switch_retire_candidate(
    chttp_service *service, const salts_service_host_generation *generation);

#ifdef __cplusplus
}
#endif
#endif
