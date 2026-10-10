#include "chttp_service_cflow_unsupported.service_native.h"
#include "chttp_service_cflow_unsupported_native.h"

int databind_7_FlowBad_4_Calc_8_Optional(
    const OptionalRequest_t *request,
    Reply_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->value = 1u;
  return 0;
}

int databind_7_FlowBad_4_Calc_8_Nullable(
    const NullableRequest_t *request,
    Reply_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->value = 2u;
  return 0;
}

int databind_7_FlowBad_4_Calc_7_Failing(
    const BaseRequest_t *request,
    Reply_t *response,
    databind_7_FlowBad_4_Calc_7_Failing__error *error) {
  if (request == NULL || response == NULL || error == NULL) return -1;
  databind_7_FlowBad_4_Calc_7_Failing__error_init(error);
  response->value = request->value;
  return 0;
}
