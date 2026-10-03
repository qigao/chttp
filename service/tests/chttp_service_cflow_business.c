#include "chttp_service_cflow.service_native.h"
#include "chttp_service_cflow_native.h"

int databind_8_CFlowGen_4_Calc_3_Add(
    const AddRequest_t *request,
    AddResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->sum = request->left + request->right;
  return 0;
}
