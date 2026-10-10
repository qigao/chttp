#include <chttp_app/service.h>
#include <chttp_app/interceptor.h>
#include <chttp_app/application.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<chttp_service>);
static_assert(std::is_standard_layout_v<chttp_application_options>);
static_assert(std::is_standard_layout_v<chttp_service_config>);
static_assert(std::is_standard_layout_v<chttp_service_http_mount>);
static_assert(std::is_standard_layout_v<chttp_service_interceptor_hook>);
static_assert(std::is_same_v<chttp_service_interceptor_before_fn,
    cmeta_status (*)(void *, const chttp_service_call *, bool *)>);
static_assert(std::is_same_v<decltype(&chttp_service_mount_http_document),
    int (*)(chttp_service *, chttp_server *, const chttp_service_http_mount *,
        chttp_service_policy_select_fn, void *)>);
static_assert(std::is_same_v<decltype(&chttp_service_mount_http_document_body),
    int (*)(chttp_service *, chttp_server *, const chttp_service_http_mount *,
        const DataBindMessagePlan *, chttp_service_policy_select_fn, void *)>);

int main() {
  static_assert(std::is_same_v<decltype(&chttp_service_mount_http_negotiated_document),
      int (*)(chttp_service *, chttp_server *, const chttp_service_http_mount *,
          const DataBindMessagePlan *, const DataBindFormatPlan *,
          chttp_service_policy_select_fn, void *)>);
  chttp_service service{};
  chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
