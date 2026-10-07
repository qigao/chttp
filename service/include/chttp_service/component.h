#ifndef CHTTP_SERVICE_COMPONENT_H
#define CHTTP_SERVICE_COMPONENT_H

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
  DataBindServiceNativeBinding native;
  DataBindNativeExecution execution;
} chttp_service_component_operation;

#define CHTTP_SERVICE_COMPONENT_OPERATION_INIT   { sizeof(chttp_service_component_operation),     DATA_BIND_SERVICE_NATIVE_BINDING_INIT(NULL, NULL, NULL),     DATA_BIND_NATIVE_EXECUTION_INIT }

static inline bool chttp_service_component_operation_valid(
    const chttp_service_component_operation *operation) {
  return operation != NULL &&
         operation->size == sizeof(*operation) &&
         data_bind_service_native_binding_valid(&operation->native) &&
         data_bind_native_execution_valid(&operation->execution);
}

#define CHTTP_SERVICE_OPERATION_PROVIDER_METHODS(X, I)   X(I, R1, bool, get_operation,     chttp_service_component_operation *, out)

CMETA_INTERFACE(
    chttp_service_operation_provider,
    CHTTP_SERVICE_OPERATION_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(chttp_service_operation_provider);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_SERVICE_COMPONENT_H */
