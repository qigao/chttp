/* Candidate-only typed Provider capability; Provider DSO does not link ServiceHost. */
#ifndef CHTTP_SERVICE_HOST_COMPONENT_H
#define CHTTP_SERVICE_HOST_COMPONENT_H

#include <data_bind_binding_plan.h>
#include <data_bind_native_binding.h>
#include <cmeta/interface.h>
#include <cmeta/object_interface.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chttp_service_component_operation {
  size_t size;
  DataBindNativeTypeBinding request;
  DataBindNativeTypeBinding response;
  DataBindServiceNativeBinding native;
  DataBindNativeExecution execution;
} chttp_service_component_operation;

#define CHTTP_SERVICE_COMPONENT_OPERATION_INIT \
  { sizeof(chttp_service_component_operation), \
    DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL), \
    DATA_BIND_NATIVE_TYPE_BINDING_INIT(NULL, NULL), \
    DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL), \
    DATA_BIND_NATIVE_EXECUTION_INIT }

static inline bool chttp_service_component_native_type_valid(
    const DataBindNativeTypeBinding *binding) {
  return binding != NULL &&
         binding->size == sizeof(*binding) &&
         binding->abi_version == DATA_BIND_NATIVE_BINDING_ABI_VERSION &&
         binding->data != NULL &&
         cmeta_data_desc_valid(binding->data);
}

static inline bool chttp_service_component_operation_valid(
    const chttp_service_component_operation *operation) {
  return operation != NULL &&
         operation->size == sizeof(*operation) &&
         chttp_service_component_native_type_valid(&operation->request) &&
         chttp_service_component_native_type_valid(&operation->response) &&
         operation->native.size == sizeof(operation->native) &&
         operation->native.abi_version == DATA_BIND_BINDING_PLAN_ABI_VERSION &&
         cmeta_function_desc_valid(operation->native.function) &&
         operation->native.request == &operation->request &&
         operation->native.response == &operation->response &&
         data_bind_native_execution_valid(&operation->execution) &&
         cmeta_function_desc_equal(
             operation->native.function, operation->execution.function);
}

/* This is a genuinely borrowed cross-DSO Interface, not an opaque R1
 * reflection shortcut. The object and pointer have a stable native identity
 * independent of the provider's address or module generation. */
static const cmeta_type_identity chttp_service_component_operation_identity =
    CMETA_TYPE_ID_ATOM_INIT("chttp.service.component.operation.v1");
static const cmeta_type_identity chttp_service_component_operation_pointer_identity =
    CMETA_TYPE_ID_POINTER_INIT(&chttp_service_component_operation_identity);
static const cmeta_type_desc chttp_service_component_operation_type = {
    "chttp_service_component_operation",
    sizeof(chttp_service_component_operation),
    CMETA_ALIGNOF(chttp_service_component_operation),
    CMETA_T_OBJECT, NULL, NULL,
    &chttp_service_component_operation_identity
};
static const cmeta_type_desc chttp_service_component_operation_pointer_type = {
    "chttp_service_component_operation *",
    sizeof(chttp_service_component_operation *),
    CMETA_ALIGNOF(chttp_service_component_operation *),
    CMETA_T_POINTER, &chttp_service_component_operation_type, NULL,
    &chttp_service_component_operation_pointer_identity
};
/* The caller owns the output record. The Provider borrows the output pointer
 * for the duration of the synchronous call; it never retains or frees it.
 * CMeta must reject this interface if return or pointer metadata is erased. */
#define CHTTP_SERVICE_OPERATION_PROVIDER_METHODS(X, I) \
  X(I, FR1, bool, get_operation, value, &cmeta_type_bool, CMETA_ABI_SCALAR, \
    CMETA_RESULT_VALUE, \
    (chttp_service_component_operation *, out, \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, \
     &chttp_service_component_operation_pointer_type, CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(
    chttp_service_operation_provider,
    CHTTP_SERVICE_OPERATION_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(chttp_service_operation_provider);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_HOST_COMPONENT_H */
