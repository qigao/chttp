#ifndef CHTTP_SERVICE_HOST_STRICT_FR_ABI_CASES_H
#define CHTTP_SERVICE_HOST_STRICT_FR_ABI_CASES_H
#include "chttp_service_host_component.h"
#include <stdio.h>
#include <stdint.h>
#define FR_CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CHttp FR1 provider ABI %s:%d: %s\n", \
            __FILE__, __LINE__, #expr); return 1; \
} } while (0)

static int verify_chttp_service_host_strict_fr1(void) {
  const cmeta_interface_desc *original =
      chttp_service_operation_provider_interface();
  const cmeta_interface_method_desc *original_method;
  const cmeta_function_desc *function;
  const cmeta_param_desc *parameter;
  const cmeta_function_abi_desc *original_abi;
  cmeta_interface_desc modified;
  cmeta_interface_method_desc method;
  cmeta_function_desc bad_function;
  cmeta_param_desc bad_parameter;
  cmeta_function_abi_desc bad_abi;
  cmeta_abi_carrier bad_carrier[1] = {CMETA_ABI_OBJECT_POINTER};

  FR_CHECK(cmeta_type_desc_valid(&chttp_service_component_operation_type));
  FR_CHECK(cmeta_type_desc_valid(&chttp_service_component_operation_pointer_type));
  FR_CHECK(cmeta_type_equal(&chttp_service_component_operation_pointer_type,
                         &chttp_service_component_operation_pointer_type));
  FR_CHECK(cmeta_interface_borrowed_service_abi_valid(original));
  FR_CHECK(original->method_count == (size_t)1u);
  original_method = &original->methods[0];
  function = original_method->function;
  original_abi = original_method->abi;
  FR_CHECK(function != NULL && original_abi != NULL);
  FR_CHECK(function->param_count == (size_t)1u);
  FR_CHECK(cmeta_type_equal(function->return_type, &cmeta_type_bool));
  FR_CHECK(original_abi->return_carrier == CMETA_ABI_SCALAR);
  FR_CHECK(function->result_flags == CMETA_RESULT_VALUE);
  parameter = &function->params[0];
  FR_CHECK(cmeta_type_equal(parameter->type,
                         &chttp_service_component_operation_pointer_type));
  FR_CHECK((parameter->flags & CMETA_PARAM_DIRECTION_MASK) == CMETA_PARAM_OUT);
  FR_CHECK((parameter->flags & CMETA_PARAM_OWNERSHIP_MASK) == CMETA_PARAM_BORROWED);
  FR_CHECK(original_abi->param_carriers[0] == CMETA_ABI_OBJECT_POINTER);

  /* These explicitly broken metadata projections must never be admitted
   * through a fallback, inferred ABI or pointer-ownership default. */
  modified = *original;
  method = *original_method;
  bad_function = *function;
  bad_parameter = *parameter;
  bad_abi = *original_abi;
  modified.methods = &method;
  method.function = &bad_function;
  method.abi = &bad_abi;
  bad_function.params = &bad_parameter;
  bad_abi.function = &bad_function;
  bad_abi.param_carriers = bad_carrier;

  bad_function.result_flags = CMETA_RESULT_UNKNOWN;
  FR_CHECK(!cmeta_interface_borrowed_service_abi_valid(&modified));
  bad_function.result_flags = CMETA_RESULT_VALUE;
  bad_parameter.flags = CMETA_PARAM_OUT;
  FR_CHECK(!cmeta_interface_borrowed_service_abi_valid(&modified));
  bad_parameter.flags = CMETA_PARAM_OUT | CMETA_PARAM_BORROWED;
  bad_carrier[0] = CMETA_ABI_UNSPECIFIED;
  FR_CHECK(!cmeta_interface_borrowed_service_abi_valid(&modified));
  bad_carrier[0] = CMETA_ABI_OBJECT_POINTER;
  bad_abi.return_carrier = CMETA_ABI_UNSPECIFIED;
  FR_CHECK(!cmeta_interface_borrowed_service_abi_valid(&modified));
  return 0;
}
#undef FR_CHECK
#endif
