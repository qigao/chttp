#include "chttp_injected_example.service_native.h"
#include <type_traits>

using Scale = int (*)(const databind_8_Injected_4_Calc_dependencies *, const Numbers_t *, Answer_t *);
static_assert(std::is_same<decltype(&databind_8_Injected_4_Calc_5_Scale), Scale>::value, "typed receiver ABI");
int main() {
  return salts_component_provider_binding_valid(databind_8_Injected_4_Calc_dependencies_component()) ? 0 : 1;
}
