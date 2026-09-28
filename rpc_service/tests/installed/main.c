#include <chttp_rpc_service/service.h>

int main(void) {
  chttp_rpc_service service = {0};
  chttp_rpc_service_config config = CHTTP_RPC_SERVICE_CONFIG_INIT;
  chttp_rpc_service_mount_options mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
  chttp_rpc_service_client_outcome outcome =
      CHTTP_RPC_SERVICE_CLIENT_OUTCOME_INIT;
  chttp_rpc_service_client_call_options call =
      CHTTP_RPC_SERVICE_CLIENT_CALL_OPTIONS_INIT;
  (void)service;
  (void)config;
  (void)mount;
  (void)outcome;
  (void)call;
  return 0;
}
