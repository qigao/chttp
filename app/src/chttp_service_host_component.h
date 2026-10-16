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

/*
 * The Provider DSO and host compile the same fully reflected native contract.
 * These TU-local descriptors have stable semantic identity across module
 * images; descriptor addresses are never an inter-DSO type identity.
 * They describe native storage, not an additional operation owner or lease.
 */
static const cmeta_type_identity chttp_service_component_operation_id =
    CMETA_TYPE_ID_ATOM_INIT("chttp.service.component_operation.v1");
static const cmeta_type_identity chttp_service_component_operation_pointer_id =
    CMETA_TYPE_ID_POINTER_INIT(&chttp_service_component_operation_id);
static const cmeta_type_desc chttp_service_component_operation_type = {
    "chttp_service_component_operation",
    sizeof(chttp_service_component_operation),
    CMETA_ALIGNOF(chttp_service_component_operation),
    CMETA_T_OBJECT, NULL, NULL, &chttp_service_component_operation_id
};
static const cmeta_type_desc chttp_service_component_operation_pointer_type = {
    "chttp_service_component_operation *",
    sizeof(chttp_service_component_operation *),
    CMETA_ALIGNOF(chttp_service_component_operation *),
    CMETA_T_POINTER,
    &chttp_service_component_operation_type,
    NULL, &chttp_service_component_operation_pointer_id
};

/* Exact bool (*)(void*, Operation*) native vtable ABI; out is caller-owned
 * and borrowed during this synchronous invocation. No dynamic ABI thunk. */
#define CHTTP_SERVICE_OPERATION_PROVIDER_METHODS(X, I) \
    X(I, FR1, bool, get_operation, stateful, &cmeta_type_bool, \
      CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, \
      (chttp_service_component_operation *, out, \
       CMETA_PARAM_OUT | CMETA_PARAM_BORROWED, \
       &chttp_service_component_operation_pointer_type, \
       CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(
    chttp_service_operation_provider,
    CHTTP_SERVICE_OPERATION_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(chttp_service_operation_provider);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_HOST_COMPONENT_H */
