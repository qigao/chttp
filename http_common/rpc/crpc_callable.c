#include "crpc_internal.h"

int crpc_bind_callable(const cmeta_callable *input, cmeta_callable *out, bool *out_present) {
  const cmeta_sig_desc *signature;
  if (out == NULL || out_present == NULL) return SALTS_EINVAL;
  *out = (cmeta_callable){0};
  *out_present = false;
  if (input == NULL) return SALTS_OK;
  if (!cmeta_callable_bind(*input, out)) return SALTS_EINVAL;
  signature = cmeta_callable_signature(*out);
  if (signature == NULL) return SALTS_EINVAL;
  if (signature->protocol != CMETA_FN_PROTOCOL_VALUE) return SALTS_ENOTSUP;
  *out_present = true;
  return SALTS_OK;
}
