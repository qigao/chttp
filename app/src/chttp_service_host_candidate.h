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
#ifdef __cplusplus
}
#endif
#endif
