#include "crypto_key_internal.h"
#include <gmssl/rsa_ext.h>
#include <stdlib.h>
#include <string.h>

static cjwt_crypto_bytes canonical(cjwt_crypto_bytes value) {
  if (!value.data) { value.size = 0; return value; }
  while (value.size > 1 && !*value.data) { ++value.data; --value.size; }
  return value;
}

void cjwt_crypto_key_destroy(cjwt_crypto_key *key) {
  if (key) { cjwt_crypto_clear(key, sizeof(*key)); free(key); }
}

cjwt_crypto_key_kind cjwt_crypto_key_type(const cjwt_crypto_key *key) {
  return key ? key->kind : 0;
}

size_t cjwt_crypto_key_signature_size(const cjwt_crypto_key *key) {
  if (!key) return 0;
  return key->kind == SALTS_CRYPTO_KEY_RSA ? key->value.rsa.size : 2 * key->scalar_size;
}

int cjwt_crypto_key_from_rsa(cjwt_crypto_bytes n, cjwt_crypto_bytes e, cjwt_crypto_bytes d,
    cjwt_crypto_bytes p, cjwt_crypto_bytes q, cjwt_crypto_bytes dp,
    cjwt_crypto_bytes dq, cjwt_crypto_bytes qi, cjwt_crypto_key **output) {
  cjwt_crypto_key *key;
  if (!output) return SALTS_CRYPTO_EINVAL;
  *output = NULL;
  if ((!n.data && n.size) || (!e.data && e.size) || (!d.data && d.size) ||
      (!p.data && p.size) || (!q.data && q.size) || (!dp.data && dp.size) ||
      (!dq.data && dq.size) || (!qi.data && qi.size)) return SALTS_CRYPTO_EINVAL;
  n = canonical(n); e = canonical(e); d = canonical(d);
  p = canonical(p); q = canonical(q); dp = canonical(dp); dq = canonical(dq); qi = canonical(qi);
  key = calloc(1, sizeof(*key));
  if (!key) return SALTS_CRYPTO_ENOMEM;
  if (rsa_components_import(&key->value.rsa, n.data, n.size, e.data, e.size, d.data, d.size) != 1)
    goto invalid;
  if (d.size && p.size && q.size && dp.size && dq.size && qi.size &&
      rsa_components_check_crt(&key->value.rsa, p.data, p.size, q.data, q.size,
        dp.data, dp.size, dq.data, dq.size, qi.data, qi.size) != 1) goto invalid;
  key->kind = SALTS_CRYPTO_KEY_RSA;
  key->has_private = key->value.rsa.has_private;
  *output = key;
  return SALTS_CRYPTO_OK;
invalid:
  cjwt_crypto_key_destroy(key);
  return SALTS_CRYPTO_EINVAL;
}

int cjwt_crypto_key_from_ec(cjwt_crypto_ec_curve curve, cjwt_crypto_bytes public_key,
    cjwt_crypto_bytes private_key, cjwt_crypto_key **output) {
  size_t n = curve == SALTS_CRYPTO_EC_P256 || curve == SALTS_CRYPTO_EC_SECP256K1 ? 32 :
             curve == SALTS_CRYPTO_EC_P384 ? 48 : curve == SALTS_CRYPTO_EC_P521 ? 66 : 0;
  cjwt_crypto_key *key;
  uint8_t derived[132];
  if (!output) return SALTS_CRYPTO_EINVAL;
  *output = NULL;
  if (!n || (!public_key.data && public_key.size) || (!private_key.data && private_key.size) ||
      (!public_key.size && !private_key.size)) return SALTS_CRYPTO_EINVAL;
  private_key = canonical(private_key);
  if (private_key.size > n) return SALTS_CRYPTO_EINVAL;
  key = calloc(1, sizeof(*key));
  if (!key) return SALTS_CRYPTO_ENOMEM;
  key->kind = SALTS_CRYPTO_KEY_EC; key->curve = curve; key->scalar_size = n;
  if (public_key.size && cjwt_crypto_ec_decode(curve, public_key.data, public_key.size,
      key->value.ec.public_key, sizeof(key->value.ec.public_key)) != SALTS_CRYPTO_OK) goto invalid;
  if (private_key.size) {
    memcpy(key->value.ec.private_key + n - private_key.size, private_key.data, private_key.size);
    if (cjwt_crypto_ecdsa_public_key(curve, key->value.ec.private_key, n, derived, 2*n) != SALTS_CRYPTO_OK)
      goto invalid;
    if (public_key.size && cjwt_crypto_equal(derived, key->value.ec.public_key, 2*n) != SALTS_CRYPTO_OK)
      goto invalid;
    memcpy(key->value.ec.public_key, derived, 2*n); key->has_private = 1;
  }
  cjwt_crypto_clear(derived, sizeof(derived));
  *output = key;
  return SALTS_CRYPTO_OK;
invalid:
  cjwt_crypto_clear(derived, sizeof(derived));
  cjwt_crypto_key_destroy(key);
  return SALTS_CRYPTO_EINVAL;
}

int cjwt_crypto_key_from_ed(cjwt_crypto_key_kind kind, cjwt_crypto_bytes public_key,
    cjwt_crypto_bytes private_key, cjwt_crypto_key **output) {
  size_t n = kind == SALTS_CRYPTO_KEY_ED25519 ? 32 : kind == SALTS_CRYPTO_KEY_ED448 ? 57 : 0;
  cjwt_crypto_key *key;
  uint8_t derived[57];
  int rc;
  if (!output) return SALTS_CRYPTO_EINVAL;
  *output = NULL;
  if (!n || (public_key.size && (!public_key.data || public_key.size != n)) ||
      (private_key.size && (!private_key.data || private_key.size != n)) ||
      (!public_key.size && !private_key.size)) return SALTS_CRYPTO_EINVAL;
  key = calloc(1, sizeof(*key));
  if (!key) return SALTS_CRYPTO_ENOMEM;
  key->kind = kind; key->scalar_size = n;
  if (public_key.size) memcpy(key->value.ec.public_key, public_key.data, n);
  if (private_key.size) {
    rc = kind == SALTS_CRYPTO_KEY_ED25519 ? cjwt_crypto_ed25519_public_key(private_key.data, derived) :
                                           cjwt_crypto_ed448_public_key(private_key.data, derived);
    if (rc != SALTS_CRYPTO_OK || (public_key.size && cjwt_crypto_equal(derived, public_key.data, n) != SALTS_CRYPTO_OK)) {
      cjwt_crypto_clear(derived, sizeof(derived)); cjwt_crypto_key_destroy(key);
      return SALTS_CRYPTO_EINVAL;
    }
    memcpy(key->value.ec.private_key, private_key.data, n);
    memcpy(key->value.ec.public_key, derived, n); key->has_private = 1;
  }
  cjwt_crypto_clear(derived, sizeof(derived));
  *output = key;
  return SALTS_CRYPTO_OK;
}

int cjwt_crypto_key_sign(const cjwt_crypto_key *key, cjwt_crypto_signature scheme,
    cjwt_crypto_hash hash, const void *message, size_t message_size,
    uint8_t *signature, size_t capacity, size_t *size) {
  uint8_t dig[64];
  size_t n = 0, required = cjwt_crypto_key_signature_size(key);
  int rc = SALTS_CRYPTO_EINVAL;
  if (size) *size = 0;
  if (!key || !key->has_private || (!message && message_size) || !signature || !size || capacity < required)
    return rc;
  if (key->kind == SALTS_CRYPTO_KEY_RSA && (scheme == SALTS_CRYPTO_RSA_PKCS1 || scheme == SALTS_CRYPTO_RSA_PSS)) {
    rc = cjwt_crypto_digest(hash, message, message_size, dig, sizeof(dig), &n);
    if (rc == SALTS_CRYPTO_OK) {
      int result = scheme == SALTS_CRYPTO_RSA_PKCS1 ?
        rsa_components_sign_pkcs1_v15_digest(&key->value.rsa, (RSA_HASH)hash, dig, n, signature, capacity, size) :
        rsa_components_sign_pss_digest(&key->value.rsa, (RSA_HASH)hash, dig, n, n, signature, capacity, size);
      rc = result == 1 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_ECRYPTO;
    }
  } else if (key->kind == SALTS_CRYPTO_KEY_EC && scheme == SALTS_CRYPTO_ECDSA) {
    rc = cjwt_crypto_ec_sign_hash(key->curve, hash, key->value.ec.private_key, key->scalar_size,
      message, message_size, signature, required);
  } else if (scheme == SALTS_CRYPTO_EDDSA) {
    if (key->kind == SALTS_CRYPTO_KEY_ED25519)
      rc = cjwt_crypto_ed25519_sign(key->value.ec.private_key, message, message_size, signature);
    else if (key->kind == SALTS_CRYPTO_KEY_ED448)
      rc = cjwt_crypto_ed448_sign(key->value.ec.private_key, message, message_size, signature);
  }
  cjwt_crypto_clear(dig, sizeof(dig));
  if (rc == SALTS_CRYPTO_OK) *size = required;
  return rc;
}

int cjwt_crypto_key_verify(const cjwt_crypto_key *key, cjwt_crypto_signature scheme,
    cjwt_crypto_hash hash, const void *message, size_t message_size,
    const uint8_t *signature, size_t size) {
  uint8_t dig[64];
  size_t n = 0;
  int rc = SALTS_CRYPTO_EINVAL;
  if (!key || (!message && message_size) || !signature) return rc;
  if (size != cjwt_crypto_key_signature_size(key)) return SALTS_CRYPTO_EVERIFY;
  if (key->kind == SALTS_CRYPTO_KEY_RSA && (scheme == SALTS_CRYPTO_RSA_PKCS1 || scheme == SALTS_CRYPTO_RSA_PSS)) {
    rc = cjwt_crypto_digest(hash, message, message_size, dig, sizeof(dig), &n);
    if (rc == SALTS_CRYPTO_OK) {
      int result = scheme == SALTS_CRYPTO_RSA_PKCS1 ?
        rsa_components_verify_pkcs1_v15_digest(&key->value.rsa, (RSA_HASH)hash, dig, n, signature, size) :
        rsa_components_verify_pss_digest(&key->value.rsa, (RSA_HASH)hash, dig, n, RSA_PSS_SALT_AUTO, signature, size);
      rc = result == 1 ? SALTS_CRYPTO_OK : result == 0 ? SALTS_CRYPTO_EVERIFY : SALTS_CRYPTO_ECRYPTO;
    }
  } else if (key->kind == SALTS_CRYPTO_KEY_EC && scheme == SALTS_CRYPTO_ECDSA) {
    rc = cjwt_crypto_ec_verify_hash(key->curve, hash, key->value.ec.public_key, 2*key->scalar_size,
      message, message_size, signature, size);
  } else if (scheme == SALTS_CRYPTO_EDDSA) {
    if (key->kind == SALTS_CRYPTO_KEY_ED25519)
      rc = cjwt_crypto_ed25519_verify(key->value.ec.public_key, message, message_size, signature);
    else if (key->kind == SALTS_CRYPTO_KEY_ED448)
      rc = cjwt_crypto_ed448_verify(key->value.ec.public_key, message, message_size, signature);
  }
  cjwt_crypto_clear(dig, sizeof(dig));
  return rc;
}

int cjwt_crypto_key_oaep_encrypt(const cjwt_crypto_key *key, cjwt_crypto_hash hash,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *output_size) {
  size_t digest_size = hash == SALTS_CRYPTO_SHA1 ? 20 : hash == SALTS_CRYPTO_SHA256 ? 32 :
    hash == SALTS_CRYPTO_SHA384 ? 48 : hash == SALTS_CRYPTO_SHA512 ? 64 : 0;
  if (output_size) *output_size = 0;
  if (!key || key->kind != SALTS_CRYPTO_KEY_RSA || !digest_size || !output || !output_size ||
      (!input && size) || capacity < key->value.rsa.size ||
      key->value.rsa.size < 2*digest_size + 2 || size > key->value.rsa.size - 2*digest_size - 2)
    return SALTS_CRYPTO_EINVAL;
  return rsa_components_oaep_encrypt(&key->value.rsa, (RSA_HASH)hash, NULL, 0, input, size,
    output, capacity, output_size) == 1 ? SALTS_CRYPTO_OK : SALTS_CRYPTO_ECRYPTO;
}

int cjwt_crypto_key_oaep_decrypt(const cjwt_crypto_key *key, cjwt_crypto_hash hash,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *output_size) {
  int rc;
  if (output_size) *output_size = 0;
  if (!key || key->kind != SALTS_CRYPTO_KEY_RSA || !key->has_private) return SALTS_CRYPTO_EINVAL;
  rc = rsa_components_oaep_decrypt(&key->value.rsa, (RSA_HASH)hash, NULL, 0, input, size,
    output, capacity, output_size);
  return rc == 1 ? SALTS_CRYPTO_OK : rc == 0 ? SALTS_CRYPTO_EVERIFY : SALTS_CRYPTO_EINVAL;
}
