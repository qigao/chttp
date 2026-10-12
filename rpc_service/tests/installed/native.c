#include <http_server/rpc_service.h>
#include <http_client/rpc_service.h>

int main(void) {
  chttp_server_rpc_config config = CHTTP_SERVER_RPC_CONFIG_INIT;
  chttp_server_rpc_mount_options mount =
      CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
  chttp_client_rpc_call_options call =
      CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  crpc_error error = {0};
  if (chttp_server_rpc_configure(NULL, &config) != SALTS_EINVAL) return 1;
  if (chttp_server_rpc_mount(NULL, &mount) != SALTS_EINVAL) return 2;
  if (chttp_client_rpc_call(NULL, &call, &outcome, &error) != SALTS_EINVAL)
    return 3;
  return 0;
}
