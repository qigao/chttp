#include "chttp_service_plugin_native.h"

int databind_11_CHttpPlugin_4_Calc_3_Add(
    const AddRequest_t *request,
    AddResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = request->left + request->right * request->scale;
  return 0;
}
