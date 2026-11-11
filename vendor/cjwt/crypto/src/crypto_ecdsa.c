#include "cjwt_crypto.h"
#include "crypto_internal.h"
#include "crypto_key_internal.h"
#include <libecc/curves/curves.h>
#include <libecc/sig/ec_key.h>
#include <libecc/sig/sig_algs.h>
#include <string.h>

static size_t ecdsa_parameters(cjwt_crypto_ec_curve curve,
                              ec_curve_type *type, hash_alg_type *hash) {
  switch (curve) {
    case SALTS_CRYPTO_EC_P256: *type = SECP256R1; *hash = SHA256; return 32;
    case SALTS_CRYPTO_EC_P384: *type = SECP384R1; *hash = SHA384; return 48;
    case SALTS_CRYPTO_EC_P521: *type = SECP521R1; *hash = SHA512; return 66;
    case SALTS_CRYPTO_EC_SECP256K1: *type = SECP256K1; *hash = SHA256; return 32;
    default: return 0;
  }
}

static int ecdsa_init(ec_curve_type type, ec_params *params) {
  const ec_str_params *description = NULL;
  memset(params, 0, sizeof(*params));
  return ec_get_curve_params_by_type(type, &description) == 0 && description &&
         import_params(params, description) == 0 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_ECRYPTO;
}

static int parameter_equal(cjwt_crypto_bytes input, const ec_str_param *expected) {
  const uint8_t *bytes = expected->buf;
  size_t size = expected->buflen;
  while (input.size > 1 && !*input.data) { ++input.data; --input.size; }
  while (size > 1 && !*bytes) { ++bytes; --size; }
  return input.size == size && memcmp(input.data, bytes, size) == 0;
}

/* Explicit parameters only admit one of the compiled named groups. The
 * descriptor remains libecc's; untrusted DER never constructs a new group. */
cjwt_crypto_ec_curve cjwt_crypto_ec_identify(cjwt_crypto_bytes p, cjwt_crypto_bytes a,
    cjwt_crypto_bytes b, cjwt_crypto_bytes generator, cjwt_crypto_bytes order,
    cjwt_crypto_bytes cofactor) {
  int c;
  for (c = SALTS_CRYPTO_EC_P256; c <= SALTS_CRYPTO_EC_SECP256K1; ++c) {
    const ec_str_params *params = NULL;
    ec_curve_type type;
    hash_alg_type hash;
    uint8_t point[132];
    size_t n = ecdsa_parameters((cjwt_crypto_ec_curve)c, &type, &hash);
    cjwt_crypto_bytes x = {point, n}, y = {point+n, n};
    if (ec_get_curve_params_by_type(type, &params) != 0 || !params ||
        !parameter_equal(p, params->p) || !parameter_equal(a, params->a) ||
        !parameter_equal(b, params->b) || !parameter_equal(order, params->gen_order) ||
        (cofactor.size && !parameter_equal(cofactor, params->cofactor))) continue;
    if (cjwt_crypto_ec_decode((cjwt_crypto_ec_curve)c, generator.data, generator.size,
          point, sizeof(point)) == SALTS_CRYPTO_OK && parameter_equal(x, params->gx) &&
        parameter_equal(y, params->gy)) return (cjwt_crypto_ec_curve)c;
  }
  return 0;
}

/* libecc's raw private-key import does not reject the zero scalar. */
static int scalar_nonzero(const uint8_t *key, size_t size) {
  uint8_t bits = 0;
  size_t i;
  for (i = 0; i < size; ++i) bits |= key[i];
  return bits != 0;
}

int cjwt_crypto_ecdsa_public_key(cjwt_crypto_ec_curve curve,
    const uint8_t *private_key, size_t private_size, uint8_t *public_key, size_t public_size) {
  ec_curve_type type;
  hash_alg_type hash;
  ec_params params;
  ec_key_pair pair;
  size_t n = ecdsa_parameters(curve, &type, &hash);
  int status;
  if (!n || !private_key || !public_key || private_size != n || public_size != 2*n)
    return SALTS_CRYPTO_EINVAL;
  if (!scalar_nonzero(private_key, n)) return SALTS_CRYPTO_EINVAL;
  memset(&pair, 0, sizeof(pair));
  status = ecdsa_init(type, &params);
  if (status == SALTS_CRYPTO_OK &&
      (ec_key_pair_import_from_priv_key_buf(&pair, &params, private_key, (u8)n, ECDSA) != 0 ||
       ec_pub_key_export_to_aff_buf(&pair.pub_key, public_key, (u8)public_size) != 0))
    status = SALTS_CRYPTO_ECRYPTO;
  cjwt_crypto_secure_zero(&pair, sizeof(pair));
  cjwt_crypto_secure_zero(&params, sizeof(params));
  if (status != SALTS_CRYPTO_OK) cjwt_crypto_secure_zero(public_key, public_size);
  return status;
}

int cjwt_crypto_ec_sign_hash(cjwt_crypto_ec_curve curve, cjwt_crypto_hash requested_hash,
    const uint8_t *private_key, size_t private_size, const void *data, size_t data_size,
    uint8_t *signature, size_t signature_size) {
  static const uint8_t empty = 0;
  ec_curve_type type;
  hash_alg_type hash;
  ec_params params;
  ec_key_pair pair;
  size_t n = ecdsa_parameters(curve, &type, &hash);
  int status;
  if (requested_hash == SALTS_CRYPTO_SHA256) hash = SHA256;
  else if (requested_hash == SALTS_CRYPTO_SHA384) hash = SHA384;
  else if (requested_hash == SALTS_CRYPTO_SHA512) hash = SHA512;
  else return SALTS_CRYPTO_EINVAL;
  if (!n || !private_key || !signature || private_size != n || signature_size != 2*n ||
      (!data && data_size) || data_size > UINT32_MAX) return SALTS_CRYPTO_EINVAL;
  if (!scalar_nonzero(private_key, n)) return SALTS_CRYPTO_EINVAL;
  memset(&pair, 0, sizeof(pair));
  status = ecdsa_init(type, &params);
  if (status == SALTS_CRYPTO_OK &&
      (ec_key_pair_import_from_priv_key_buf(&pair, &params, private_key, (u8)n, ECDSA) != 0 ||
       ec_sign(signature, (u8)signature_size, &pair, data ? data : &empty,
               (u32)data_size, ECDSA, hash, NULL, 0) != 0)) status = SALTS_CRYPTO_ECRYPTO;
  cjwt_crypto_secure_zero(&pair, sizeof(pair));
  cjwt_crypto_secure_zero(&params, sizeof(params));
  if (status != SALTS_CRYPTO_OK) cjwt_crypto_secure_zero(signature, signature_size);
  return status;
}

int cjwt_crypto_ec_verify_hash(cjwt_crypto_ec_curve curve, cjwt_crypto_hash requested_hash,
    const uint8_t *public_key, size_t public_size, const void *data, size_t data_size,
    const uint8_t *signature, size_t signature_size) {
  static const uint8_t empty = 0;
  ec_curve_type type;
  hash_alg_type hash;
  ec_params params;
  ec_pub_key key;
  size_t n = ecdsa_parameters(curve, &type, &hash);
  int status;
  if (requested_hash == SALTS_CRYPTO_SHA256) hash = SHA256;
  else if (requested_hash == SALTS_CRYPTO_SHA384) hash = SHA384;
  else if (requested_hash == SALTS_CRYPTO_SHA512) hash = SHA512;
  else return SALTS_CRYPTO_EINVAL;
  if (!n || !public_key || !signature || public_size != 2*n || signature_size != 2*n ||
      (!data && data_size) || data_size > UINT32_MAX) return SALTS_CRYPTO_EINVAL;
  memset(&key, 0, sizeof(key));
  status = ecdsa_init(type, &params);
  if (status == SALTS_CRYPTO_OK &&
      (ec_pub_key_import_from_aff_buf(&key, &params, public_key, (u8)public_size, ECDSA) != 0 ||
       ec_verify(signature, (u8)signature_size, &key, data ? data : &empty,
                 (u32)data_size, ECDSA, hash, NULL, 0) != 0)) status = SALTS_CRYPTO_EVERIFY;
  cjwt_crypto_secure_zero(&key, sizeof(key));
  cjwt_crypto_secure_zero(&params, sizeof(params));
  return status;
}

int cjwt_crypto_ecdsa_sign(cjwt_crypto_ec_curve curve,
    const uint8_t *key, size_t key_size, const void *message, size_t size,
    uint8_t *signature, size_t signature_size) {
  cjwt_crypto_hash hash = curve == SALTS_CRYPTO_EC_P384 ? SALTS_CRYPTO_SHA384 :
    curve == SALTS_CRYPTO_EC_P521 ? SALTS_CRYPTO_SHA512 : SALTS_CRYPTO_SHA256;
  return cjwt_crypto_ec_sign_hash(curve, hash, key, key_size, message, size, signature, signature_size);
}

int cjwt_crypto_ecdsa_verify(cjwt_crypto_ec_curve curve,
    const uint8_t *key, size_t key_size, const void *message, size_t size,
    const uint8_t *signature, size_t signature_size) {
  cjwt_crypto_hash hash = curve == SALTS_CRYPTO_EC_P384 ? SALTS_CRYPTO_SHA384 :
    curve == SALTS_CRYPTO_EC_P521 ? SALTS_CRYPTO_SHA512 : SALTS_CRYPTO_SHA256;
  return cjwt_crypto_ec_verify_hash(curve, hash, key, key_size, message, size, signature, signature_size);
}

int cjwt_crypto_ec_decode(cjwt_crypto_ec_curve curve, const uint8_t *input, size_t size,
    uint8_t *output, size_t capacity) {
  ec_curve_type type;
  hash_alg_type hash;
  ec_params params;
  ec_pub_key key;
  fp x = {0}, y1 = {0}, y2 = {0};
  uint8_t decoded[132], other[66];
  size_t n = ecdsa_parameters(curve, &type, &hash);
  int status = SALTS_CRYPTO_EINVAL;
  if (!n || !input || !size || !output || capacity < 2*n) return status;
  memset(&params, 0, sizeof(params)); memset(&key, 0, sizeof(key));
  if (ecdsa_init(type, &params) != SALTS_CRYPTO_OK) goto end;
  if ((input[0] == 4 || input[0] == 6 || input[0] == 7) && size == 1 + 2*n) {
    memcpy(decoded, input + 1, 2*n);
    if (input[0] != 4 && ((decoded[2*n-1] ^ input[0]) & 1u)) goto end;
  } else if ((input[0] == 2 || input[0] == 3) && size == n + 1) {
    if (fp_init_from_buf(&x, &params.ec_fp, input + 1, (u16)n) != 0 ||
        aff_pt_y_from_x(&y1, &y2, &x, &params.ec_curve) != 0 ||
        fp_export_to_buf(decoded + n, (u16)n, &y1) != 0 ||
        fp_export_to_buf(other, (u16)n, &y2) != 0) goto end;
    memcpy(decoded, input + 1, n);
    if ((decoded[2*n-1] ^ input[0]) & 1u) memcpy(decoded + n, other, n);
  } else goto end;
  if (ec_pub_key_import_from_aff_buf(&key, &params, decoded, (u8)(2*n), ECDSA) != 0) goto end;
  memcpy(output, decoded, 2*n);
  status = SALTS_CRYPTO_OK;
end:
  cjwt_crypto_secure_zero(&params, sizeof(params)); cjwt_crypto_secure_zero(&key, sizeof(key));
  cjwt_crypto_secure_zero(&x, sizeof(x)); cjwt_crypto_secure_zero(&y1, sizeof(y1));
  cjwt_crypto_secure_zero(&y2, sizeof(y2)); cjwt_crypto_secure_zero(decoded, sizeof(decoded));
  cjwt_crypto_secure_zero(other, sizeof(other));
  return status;
}
