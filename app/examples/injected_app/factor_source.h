#ifndef CHTTP_EXAMPLE_FACTOR_SOURCE_H
#define CHTTP_EXAMPLE_FACTOR_SOURCE_H
#include <cmeta/interface.h>
#include <cmeta/data_reflect.h>
#include <salts/component_abi.h>

#define FACTOR_SOURCE_METHODS(X, I) X(I, R0, uint32_t, value, _)
CMETA_INTERFACE(FactorSource, FACTOR_SOURCE_METHODS);
CMETA_INTERFACE(OffsetSource, FACTOR_SOURCE_METHODS);

typedef struct FactorSettings { uint32_t factor; } FactorSettings;
cmeta_reflect_data(FactorSettings, "chttp.example.FactorSettings",
    cmeta_data_field(uint32_t, factor, &cmeta_data_uint32, &cmeta_type_uint32));

#ifdef __cplusplus
extern "C" {
#endif
const salts_component_provider_binding *factor_source_provider(void);
#ifdef __cplusplus
}
#endif
#endif
