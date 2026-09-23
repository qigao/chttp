#include <chttp_service/service.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_service>);
static_assert(std::is_standard_layout_v<chttp_service_config>);
static_assert(std::is_standard_layout_v<chttp_service_context>);
static_assert(std::is_standard_layout_v<chttp_service_result>);
static_assert(std::is_standard_layout_v<chttp_service_http_method>);

int main() {
  chttp_service service{};
  chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_context context = CHTTP_SERVICE_CONTEXT_INIT;
  chttp_service_result result = CHTTP_SERVICE_RESULT_INIT;
  chttp_service_http_method method = CHTTP_SERVICE_HTTP_METHOD_INIT;
  (void)service;
  (void)config;
  (void)context;
  (void)result;
  (void)method;
  return 0;
}
