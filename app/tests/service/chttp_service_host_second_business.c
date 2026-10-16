#include "chttp_service_plugin_native.h"

/* Same operation contract, distinct response from the second DSO. */
int databind_11_CHttpPlugin_4_Calc_3_Add(
    const AddRequest_t *request, AddResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = 100u + request->left + request->right * request->scale;
  return 0;
}
