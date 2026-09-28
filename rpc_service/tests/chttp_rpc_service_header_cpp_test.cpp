#include <chttp_rpc_service/service.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_rpc_service>);
static_assert(std::is_standard_layout_v<chttp_rpc_service_config>);
static_assert(std::is_standard_layout_v<chttp_rpc_service_mount_options>);

int main() {
  chttp_rpc_service service{};
  chttp_rpc_service_config config = CHTTP_RPC_SERVICE_CONFIG_INIT;
  chttp_rpc_service_mount_options mount = CHTTP_RPC_SERVICE_MOUNT_OPTIONS_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
