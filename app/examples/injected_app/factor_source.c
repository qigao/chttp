#include "factor_source.h"
#include <stdlib.h>

cmeta_component_configured(FactorProvider, cmeta_reflected_data(FactorSettings),
    cmeta_provides(FactorSource) cmeta_provides(OffsetSource));
static uint32_t factor_value(void *self) { return ((FactorSettings *)self)->factor; }
CMETA_IMPLEMENTS(FactorSource, factor_source, 0u, .value = factor_value);
CMETA_IMPLEMENTS(OffsetSource, offset_source, 0u, .value = factor_value);

static cmeta_status factor_project(void *context, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
  (void)context;
  if (cmeta_interface_desc_equal(expected, FactorSource_interface()))
    *out = (cmeta_interface_projection){sizeof(*out), FactorSource_interface(), object->object, &factor_source_vtable};
  else if (cmeta_interface_desc_equal(expected, OffsetSource_interface()))
    *out = (cmeta_interface_projection){sizeof(*out), OffsetSource_interface(), object->object, &offset_source_vtable};
  else return CMETA_TRAIT_MISSING;
  return CMETA_OK;
}
static const cmeta_object_interface_provider interfaces = {sizeof(interfaces), NULL, factor_project};
static void factor_destroy(void *context, void *object) { (void)context; free(object); }
static const cmeta_object_lifecycle lifecycle = {sizeof(lifecycle), NULL, NULL, NULL, factor_destroy};
static cmeta_status SALTS_COMPONENT_CALL factor_create(void *context, const cmeta_data_desc *data,
    const void *config, const salts_component_dependency *dependencies, size_t count, cmeta_object_ref *out) {
  (void)context; (void)dependencies;
  if (count != 0u || config == NULL || !cmeta_data_desc_equal(data, cmeta_reflected_data(FactorSettings)))
    return CMETA_INVALID_ARGUMENT;
  FactorSettings *instance = malloc(sizeof(*instance));
  if (instance == NULL) return CMETA_OUT_OF_MEMORY;
  *instance = *(const FactorSettings *)config;
  cmeta_status status = cmeta_object_borrow(out, instance, cmeta_reflected_data(FactorSettings), NULL);
  if (status == CMETA_OK) status = cmeta_object_take(out, &lifecycle);
  if (status != CMETA_OK) { cmeta_object_release(out); free(instance); }
  return status;
}
const salts_component_provider_binding *factor_source_provider(void) {
  static const salts_component_provider_binding provider = {sizeof(provider), SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
      cmeta_component_meta(FactorProvider), NULL, &interfaces, factor_create, NULL, NULL};
  return &provider;
}
