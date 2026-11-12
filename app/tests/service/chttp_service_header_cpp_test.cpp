#include "../../src/chttp_method_runtime.h"

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_method_runtime>);
static_assert(std::is_standard_layout_v<chttp_method_runtime_config>);
static_assert(std::is_standard_layout_v<chttp_method_runtime_http_mount>);

int main() {
  chttp_method_runtime service{};
  chttp_method_runtime_config config = CHTTP_METHOD_RUNTIME_CONFIG_INIT;
  chttp_method_runtime_http_mount mount = CHTTP_METHOD_RUNTIME_HTTP_MOUNT_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
