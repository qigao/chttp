#include "chttp_policy_fixture.service_native.h"
int databind_9_PolicyApp_3_Api_4_Echo(const Request_t *request, Reply_t *response) {
  response->value = request->value;
  return 0;
}
