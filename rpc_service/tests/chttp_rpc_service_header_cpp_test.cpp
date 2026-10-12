#include <http_server/rpc_service.h>
#include <http_client/rpc_service.h>
#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_server_rpc_config>);
static_assert(std::is_standard_layout_v<chttp_server_rpc_mount_options>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_call_options>);
static_assert(std::is_standard_layout_v<chttp_client_rpc_outcome>);

int main() {
  chttp_server_rpc_config config = CHTTP_SERVER_RPC_CONFIG_INIT;
  chttp_server_rpc_mount_options mount = CHTTP_SERVER_RPC_MOUNT_OPTIONS_INIT;
  chttp_client_rpc_call_options call = CHTTP_CLIENT_RPC_CALL_OPTIONS_INIT;
  chttp_client_rpc_outcome outcome = CHTTP_CLIENT_RPC_OUTCOME_INIT;
  (void)config; (void)mount; (void)call; (void)outcome;
  return 0;
}
