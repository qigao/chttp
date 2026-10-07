#include <chttp_service/component.h>

#include "chttp_service_plugin_native.h"

#include <salts/component_plugin_abi.h>
#include <salts/plugin_decl.h>

#define CHTTP_COMPONENT_PROVIDER_EXPORT "component-provider"

cmeta_component(CHttpPlugin_Calc_Add,
    cmeta_provides(chttp_service_operation_provider));

static int chttp_component_state;

static bool chttp_component_get_operation(
    void *self,
    chttp_service_component_operation *out) {
  const DataBindNativeExecution *execution;
  DataBindError error = DATA_BIND_ERROR_INIT;

  (void)self;
  if (out == NULL) return false;

  *out =
      (chttp_service_component_operation)CHTTP_SERVICE_COMPONENT_OPERATION_INIT;

  if (databind_11_CHttpPlugin_4_Calc_3_Add__databind_native_binding(
          &out->request, &out->response, &out->native, &error) != DATA_BIND_OK)
    return false;

  execution =
      databind_11_CHttpPlugin_4_Calc_3_Add__databind_execution();
  if (execution == NULL || !data_bind_native_execution_valid(execution))
    return false;

  out->execution = *execution;
  return chttp_service_component_operation_valid(out);
}

CMETA_IMPLEMENTS(
    chttp_service_operation_provider,
    chttp_component_operation_impl,
    0u,
    .get_operation = chttp_component_get_operation);

static cmeta_status chttp_component_project(
    void *context,
    const cmeta_object_ref *object,
    const cmeta_interface_desc *expected,
    cmeta_interface_projection *out) {
  (void)context;
  if (object == NULL || out == NULL)
    return CMETA_INVALID_ARGUMENT;
  if (!cmeta_interface_desc_equal(
          expected, chttp_service_operation_provider_interface()))
    return CMETA_TRAIT_MISSING;

  out->size = sizeof(*out);
  out->interface = chttp_service_operation_provider_interface();
  out->self = object->object;
  out->dispatch = &chttp_component_operation_impl_vtable;
  return CMETA_OK;
}

static const cmeta_object_interface_provider chttp_component_interfaces = {
    sizeof(cmeta_object_interface_provider),
    NULL,
    chttp_component_project
};

static cmeta_status SALTS_COMPONENT_CALL chttp_component_create(
    void *provider_context,
    const cmeta_data_desc *config_data,
    const void *config_value,
    const salts_component_dependency *dependencies,
    size_t dependency_count,
    cmeta_object_ref *out_instance) {
  (void)config_data;
  (void)config_value;
  (void)dependencies;
  if (provider_context == NULL || dependency_count != 0u)
    return CMETA_INVALID_ARGUMENT;
  return cmeta_object_borrow(
      out_instance, provider_context, &cmeta_data_int, NULL);
}

static const salts_component_provider_binding chttp_component_binding = {
    sizeof(salts_component_provider_binding),
    SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(CHttpPlugin_Calc_Add),
    &chttp_component_state,
    &chttp_component_interfaces,
    chttp_component_create,
    NULL,
    NULL
};

static const salts_component_provider_binding *
chttp_component_provider_binding(void *self) {
  return (const salts_component_provider_binding *)self;
}

CMETA_IMPLEMENTS(
    salts_component_provider,
    chttp_component_provider_impl,
    0u,
    .get_binding = chttp_component_provider_binding);

static salts_component_provider chttp_component_provider = {
    (void *)&chttp_component_binding,
    &chttp_component_provider_impl_vtable
};

#define CHTTP_COMPONENT_EXPORTS(X) \
  X(interface, (salts_component_provider, &chttp_component_provider), \
    CHTTP_COMPONENT_PROVIDER_EXPORT, \
    SALTS_COMPONENT_PROVIDER_CONTRACT_ID, \
    SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION, 0)

CMETA_PLUGIN_DECLARE(
    chttp_component_fixture,
    "test.chttp.component-provider",
    (1,0,0),
    CHTTP_COMPONENT_EXPORTS,
    CMETA_PLUGIN_PASSIVE());
