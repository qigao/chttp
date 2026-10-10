#include "chttp_service_generic_identity.h"
#include "chttp_service_generic_native.h"

#include <cmeta/data.h>
#include <cmeta/declared_type.h>
#include <cmeta/struct.h>

#include <string.h>

typedef struct chttp_service_generic_identity_state {
  cmeta_generic_desc constructor;
  const cmeta_type_identity *arguments[2];
  cmeta_type_identity identity;
} chttp_service_generic_identity_state;

static int build_identity(
    const char *field_name, chttp_service_generic_identity_state *state) {
  const cmeta_data_desc *data = NULL;
  const cmeta_data_struct_shape *shape;
  const cmeta_field_desc *field;
  const cmeta_declared_type *declared;
  DataBindError error = DATA_BIND_ERROR_INIT;
  size_t i;

  if (field_name == NULL || state == NULL ||
      GenericEnvelope_cmeta_data(&data, &error) != DATA_BIND_OK ||
      data == NULL || data->kind != CMETA_DATA_STRUCT || data->shape == NULL)
    return 0;

  shape = (const cmeta_data_struct_shape *)data->shape;
  if (shape->layout == NULL) return 0;
  field = cmeta_struct_find_field(shape->layout, field_name);
  if (field == NULL || field->declared_type == NULL) return 0;
  declared = field->declared_type;
  if (!cmeta_declared_type_valid(declared) ||
      declared->constructor == NULL || declared->arity == 0u ||
      declared->arity > 2u)
    return 0;

  state->constructor = *declared->constructor;
  for (i = 0u; i < declared->arity; ++i) {
    const cmeta_type_desc *argument = cmeta_declared_type_argument(declared, i);
    state->arguments[i] = cmeta_type_identity_of(argument);
    if (state->arguments[i] == NULL) return 0;
  }
  state->identity = (cmeta_type_identity){
      CMETA_TYPE_APPLY, NULL, &state->constructor, NULL,
      state->arguments, declared->arity};
  return cmeta_type_identity_valid(&state->identity);
}

const cmeta_type_identity *chttp_service_generic_identity_a(
    const char *field_name, const cmeta_generic_desc **out_constructor_copy) {
  static chttp_service_generic_identity_state values_state;
  static chttp_service_generic_identity_state ids_state;
  chttp_service_generic_identity_state *state =
      strcmp(field_name != NULL ? field_name : "", "values") == 0
          ? &values_state
          : &ids_state;
  if (!build_identity(field_name, state)) return NULL;
  if (out_constructor_copy != NULL) *out_constructor_copy = &state->constructor;
  return &state->identity;
}
