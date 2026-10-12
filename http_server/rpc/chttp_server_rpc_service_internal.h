#ifndef CHTTP_SERVER_RPC_SERVICE_INTERNAL_H
#define CHTTP_SERVER_RPC_SERVICE_INTERNAL_H

#include <http_server/rpc_service.h>

/* Private staged generated adapter. The owning CRPC registry controls it and
 * frees it only after the host HTTP server has drained all callbacks. */
typedef struct chttp_server_rpc_generated_config {
  size_t method_capacity;
  size_t max_output_value_bytes;
  size_t max_call_frame_bytes;
  size_t native_workspace_bytes;
  size_t native_max_depth;
  size_t native_max_items;
  size_t native_max_owned_bytes;
  size_t max_inflight_calls;
} chttp_server_rpc_generated_config;

int chttp_server_rpc_generated_init(
    void **out_context, const chttp_server_rpc_generated_config *config);
int chttp_server_rpc_generated_mount(
    void *context, chttp_server *server,
    const chttp_server_rpc_mount_options *mount);
void chttp_server_rpc_generated_release(void *context);

#endif /* CHTTP_SERVER_RPC_SERVICE_INTERNAL_H */
