#include <chttp_service/service.h>

int main(void) {
  chttp_service service = {0};
  chttp_service_config config = CHTTP_SERVICE_CONFIG_INIT;
  chttp_service_http_mount mount = CHTTP_SERVICE_HTTP_MOUNT_INIT;
  config.method_capacity = 1u;
  if (chttp_service_init(&service, &config) != SALTS_OK) return 1;
  if (chttp_service_mount_http(&service, NULL, &mount) != SALTS_EINVAL) {
    chttp_service_destroy(&service);
    return 2;
  }
  return chttp_service_destroy(&service) == SALTS_OK ? 0 : 3;
}
