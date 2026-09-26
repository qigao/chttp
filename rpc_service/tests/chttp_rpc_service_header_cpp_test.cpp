#include <chttp_rpc_service/service.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_rpc_service>);
static_assert(std::is_standard_layout_v<chttp_rpc_service_config>);
static_assert(std::is_standard_layout_v<chttp_rpc_service_mount>);

int main() {
  chttp_rpc_service service{};
  chttp_rpc_service_config config = CHTTP_RPC_SERVICE_CONFIG_INIT;
  chttp_rpc_service_mount mount = CHTTP_RPC_SERVICE_MOUNT_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
