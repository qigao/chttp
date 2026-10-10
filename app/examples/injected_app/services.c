#include "chttp_injected_example.service_native.h"

int databind_8_Injected_4_Calc_5_Scale(const databind_8_Injected_4_Calc_dependencies *dependencies,
    const Numbers_t *request, Answer_t *response) {
  FactorSource factor = dependencies->factor;
  response->value = (uint64_t)request->value * FactorSource_value(&factor);
  return 0;
}

int databind_8_Injected_4_Calc_4_Next(const databind_8_Injected_4_Calc_dependencies *dependencies,
    const Numbers_t *request, Answer_t *response) {
  OffsetSource offset = dependencies->offset;
  response->value = (uint64_t)request->value + OffsetSource_value(&offset);
  return 0;
}
