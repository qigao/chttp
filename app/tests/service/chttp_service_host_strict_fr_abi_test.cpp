#include "chttp_service_host_strict_fr_abi_cases.h"
#include <type_traits>
static_assert(std::is_standard_layout<chttp_service_component_operation>::value,
              "MethodPlan out record must keep standard C layout");
static_assert(std::is_standard_layout<chttp_service_operation_provider>::value,
              "Reflected Provider ABI must keep standard C layout");
int main() { return verify_chttp_service_host_strict_fr1(); }
