#include "chttp_service_wasm_native.h"

int databind_9_CHttpWasm_4_Calc_3_Add(
    const AddRequest_t *request,
    AddResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  if (request->left == 99u) return 7;
  response->sum = request->left + request->right * request->scale;
  return 0;
}
