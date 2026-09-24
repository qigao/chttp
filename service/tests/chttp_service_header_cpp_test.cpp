#include <chttp_service/service.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_service>);
static_assert(std::is_standard_layout_v<chttp_service_config>);
static_assert(std::is_standard_layout_v<chttp_service_http_mount>);

int main() {
  chttp_service service{};
  chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
