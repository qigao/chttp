#include "chttp_service_generic_identity.h"

#include <cmeta/type_identity.h>

#include <stdio.h>

int main(void) {
  const cmeta_generic_desc *vec_a = NULL;
  const cmeta_generic_desc *vec_b = NULL;
  const cmeta_generic_desc *set_a = NULL;
  const cmeta_type_identity *values_a =
      chttp_service_generic_identity_a("values", &vec_a);
  const cmeta_type_identity *values_b =
      chttp_service_generic_identity_b("values", &vec_b);
  const cmeta_type_identity *ids_a =
      chttp_service_generic_identity_a("ids", &set_a);

  if (values_a == NULL || values_b == NULL || ids_a == NULL ||
      vec_a == NULL || vec_b == NULL || set_a == NULL) {
    fputs("missing generated generic identity\n", stderr);
    return 1;
  }

  if (vec_a == vec_b) {
    fputs("cross-TU constructor copies unexpectedly share an address\n", stderr);
    return 2;
  }

  if (!cmeta_type_identity_equal(values_a, values_b)) {
    fputs("same generated generic application is not semantically equal\n", stderr);
    return 3;
  }

  if (cmeta_type_identity_equal(values_a, ids_a)) {
    fputs("different generic constructors compare equal\n", stderr);
    return 4;
  }

  if (values_a->constructor == values_b->constructor) {
    fputs("APPLY identities unexpectedly reuse constructor address\n", stderr);
    return 5;
  }

  if (values_a->arity != 1u || values_b->arity != 1u || ids_a->arity != 1u) {
    fputs("unexpected generated generic arity\n", stderr);
    return 6;
  }

  return 0;
}
