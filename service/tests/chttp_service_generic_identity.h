#ifndef CHTTP_SERVICE_GENERIC_IDENTITY_H
#define CHTTP_SERVICE_GENERIC_IDENTITY_H

#include <cmeta/type_identity.h>

#ifdef __cplusplus
extern "C" {
#endif

const cmeta_type_identity *chttp_service_generic_identity_a(
    const char *field_name, const cmeta_generic_desc **out_constructor_copy);
const cmeta_type_identity *chttp_service_generic_identity_b(
    const char *field_name, const cmeta_generic_desc **out_constructor_copy);

#ifdef __cplusplus
}
#endif

#endif
