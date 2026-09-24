#include <chttp_service/service.h>

int main(void) {
  chttp_service service = {0};
  chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  (void)service;
  (void)config;
  (void)mount;
  return 0;
}
