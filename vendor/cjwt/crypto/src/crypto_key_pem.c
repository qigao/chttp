#include "crypto_key_internal.h"
#include <stdio.h>
#include <gmssl/asn1.h>
#include <gmssl/base64.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *data; size_t size; } der_reader;

static int take(der_reader *in, int tag, der_reader *value) {
  return asn1_type_from_der(tag, &value->data, &value->size, &in->data, &in->size) == 1;
}
static int integer(der_reader *in, cjwt_crypto_bytes *value) {
  return asn1_integer_from_der(&value->data, &value->size, &in->data, &in->size) == 1;
}
static int sequence(der_reader input, der_reader *value) {
  return take(&input, ASN1_TAG_SEQUENCE, value) && input.size == 0;
}
static int bytes_equal(der_reader value, const uint8_t *bytes, size_t size) {
  return value.size == size && memcmp(value.data, bytes, size) == 0;
}
static cjwt_crypto_ec_curve curve_oid(der_reader value) {
  static const uint8_t p256[] = {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
  static const uint8_t p384[] = {0x2b,0x81,0x04,0x00,0x22};
  static const uint8_t p521[] = {0x2b,0x81,0x04,0x00,0x23};
  static const uint8_t k256[] = {0x2b,0x81,0x04,0x00,0x0a};
  if (bytes_equal(value, p256, sizeof(p256))) return SALTS_CRYPTO_EC_P256;
  if (bytes_equal(value, p384, sizeof(p384))) return SALTS_CRYPTO_EC_P384;
  if (bytes_equal(value, p521, sizeof(p521))) return SALTS_CRYPTO_EC_P521;
  if (bytes_equal(value, k256, sizeof(k256))) return SALTS_CRYPTO_EC_SECP256K1;
  return 0;
}

static cjwt_crypto_ec_curve curve_parameters(der_reader *input) {
  static const uint8_t prime_field[] = {0x2a,0x86,0x48,0xce,0x3d,0x01,0x01};
  der_reader seq, field, oid, coefficients, a, b, base, seed;
  cjwt_crypto_bytes p, order, cofactor = {0};
  int version;
  if (!input->size) return 0;
  if (input->data[0] == ASN1_TAG_OBJECT_IDENTIFIER)
    return take(input, ASN1_TAG_OBJECT_IDENTIFIER, &oid) ? curve_oid(oid) : 0;
  if (!take(input, ASN1_TAG_SEQUENCE, &seq) ||
      asn1_int_from_der(&version, &seq.data, &seq.size) != 1 || version != 1 ||
      !take(&seq, ASN1_TAG_SEQUENCE, &field) || !take(&field, ASN1_TAG_OBJECT_IDENTIFIER, &oid) ||
      !bytes_equal(oid, prime_field, sizeof(prime_field)) || !integer(&field, &p) || field.size ||
      !take(&seq, ASN1_TAG_SEQUENCE, &coefficients) ||
      !take(&coefficients, ASN1_TAG_OCTET_STRING, &a) ||
      !take(&coefficients, ASN1_TAG_OCTET_STRING, &b)) return 0;
  if (coefficients.size && (!take(&coefficients, ASN1_TAG_BIT_STRING, &seed) ||
      !seed.size || seed.data[0] > 7)) return 0;
  if (coefficients.size || !take(&seq, ASN1_TAG_OCTET_STRING, &base) ||
      !integer(&seq, &order) || (seq.size && !integer(&seq, &cofactor)) || seq.size) return 0;
  {
    cjwt_crypto_bytes av = {a.data, a.size}, bv = {b.data, b.size}, g = {base.data, base.size};
    return cjwt_crypto_ec_identify(p, av, bv, g, order, cofactor);
  }
}

static int algorithm(der_reader *input, cjwt_crypto_key_kind *kind, cjwt_crypto_ec_curve *curve) {
  static const uint8_t rsa[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
  static const uint8_t ec[] = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
  static const uint8_t ed25519[] = {0x2b,0x65,0x70}, ed448[] = {0x2b,0x65,0x71};
  der_reader alg, oid, params;
  if (!take(input, ASN1_TAG_SEQUENCE, &alg) || !take(&alg, ASN1_TAG_OBJECT_IDENTIFIER, &oid)) return 0;
  *curve = 0;
  if (bytes_equal(oid, rsa, sizeof(rsa))) {
    *kind = SALTS_CRYPTO_KEY_RSA;
    if (alg.size && (!take(&alg, ASN1_TAG_NULL, &params) || params.size)) return 0;
  } else if (bytes_equal(oid, ec, sizeof(ec))) {
    *kind = SALTS_CRYPTO_KEY_EC;
    if (!(*curve = curve_parameters(&alg))) return 0;
  } else if (bytes_equal(oid, ed25519, sizeof(ed25519))) *kind = SALTS_CRYPTO_KEY_ED25519;
  else if (bytes_equal(oid, ed448, sizeof(ed448))) *kind = SALTS_CRYPTO_KEY_ED448;
  else return 0;
  return !alg.size;
}

static int rsa_der(der_reader input, int private_key, cjwt_crypto_key **output) {
  cjwt_crypto_bytes values[8] = {{0}};
  der_reader seq;
  size_t i, count = private_key ? 8 : 2;
  int version;
  if (!sequence(input, &seq)) return SALTS_CRYPTO_EINVAL;
  if (private_key && (asn1_int_from_der(&version, &seq.data, &seq.size) != 1 || version != 0))
    return SALTS_CRYPTO_EINVAL;
  for (i = 0; i < count; ++i) if (!integer(&seq, &values[i])) return SALTS_CRYPTO_EINVAL;
  if (seq.size) return SALTS_CRYPTO_EINVAL;
  return cjwt_crypto_key_from_rsa(values[0], values[1], values[2], values[3], values[4],
    values[5], values[6], values[7], output);
}

static int ec_private_der(der_reader input, cjwt_crypto_ec_curve curve, cjwt_crypto_key **output) {
  der_reader seq, secret, wrapper;
  cjwt_crypto_bytes pub = {0}, priv;
  cjwt_crypto_ec_curve inner;
  int version;
  if (!sequence(input, &seq) || asn1_int_from_der(&version, &seq.data, &seq.size) != 1 || version != 1 ||
      !take(&seq, ASN1_TAG_OCTET_STRING, &secret)) return SALTS_CRYPTO_EINVAL;
  if (seq.size && seq.data[0] == 0xa0) {
    if (!take(&seq, 0xa0, &wrapper) || !(inner = curve_parameters(&wrapper)) || wrapper.size ||
        (curve && curve != inner)) return SALTS_CRYPTO_EINVAL;
    curve = inner;
  }
  if (seq.size && seq.data[0] == 0xa1) {
    if (!take(&seq, 0xa1, &wrapper) ||
        asn1_bit_octets_from_der(&pub.data, &pub.size, &wrapper.data, &wrapper.size) != 1 || wrapper.size)
      return SALTS_CRYPTO_EINVAL;
  }
  if (seq.size || !curve) return SALTS_CRYPTO_EINVAL;
  priv.data = secret.data; priv.size = secret.size;
  return cjwt_crypto_key_from_ec(curve, pub, priv, output);
}

static int key_info_der(der_reader input, int private_key, cjwt_crypto_key **output) {
  der_reader seq, payload, inner, attrs;
  cjwt_crypto_key_kind kind;
  cjwt_crypto_ec_curve curve;
  cjwt_crypto_bytes pub = {0}, priv = {0};
  int version;
  if (!sequence(input, &seq)) return SALTS_CRYPTO_EINVAL;
  if (private_key && (asn1_int_from_der(&version, &seq.data, &seq.size) != 1 || version != 0))
    return SALTS_CRYPTO_EINVAL;
  if (!algorithm(&seq, &kind, &curve)) return SALTS_CRYPTO_EINVAL;
  if (private_key) {
    if (!take(&seq, ASN1_TAG_OCTET_STRING, &payload)) return SALTS_CRYPTO_EINVAL;
    if (seq.size && !take(&seq, 0xa0, &attrs)) return SALTS_CRYPTO_EINVAL;
  } else if (asn1_bit_octets_from_der(&payload.data, &payload.size, &seq.data, &seq.size) != 1)
    return SALTS_CRYPTO_EINVAL;
  if (seq.size) return SALTS_CRYPTO_EINVAL;
  if (kind == SALTS_CRYPTO_KEY_RSA) return rsa_der(payload, private_key, output);
  if (kind == SALTS_CRYPTO_KEY_EC) {
    if (private_key) return ec_private_der(payload, curve, output);
    pub.data = payload.data; pub.size = payload.size;
    return cjwt_crypto_key_from_ec(curve, pub, priv, output);
  }
  if (private_key) {
    if (!take(&payload, ASN1_TAG_OCTET_STRING, &inner) || payload.size) return SALTS_CRYPTO_EINVAL;
    priv.data = inner.data; priv.size = inner.size;
  } else { pub.data = payload.data; pub.size = payload.size; }
  return cjwt_crypto_key_from_ed(kind, pub, priv, output);
}

static const uint8_t *find_marker(const uint8_t *data, size_t size, const char *marker) {
  size_t i, n = strlen(marker);
  if (size < n) return NULL;
  for (i = 0; i <= size - n; ++i)
    if (data[i] == '-' && memcmp(data + i, marker, n) == 0) return data + i;
  return NULL;
}

int cjwt_crypto_key_from_pem(const void *pem, size_t size, int private_key, cjwt_crypto_key **output) {
  static const struct { const char *begin, *end; int type, private_key; } formats[] = {
    {"-----BEGIN PUBLIC KEY-----", "-----END PUBLIC KEY-----", 0, 0},
    {"-----BEGIN PRIVATE KEY-----", "-----END PRIVATE KEY-----", 0, 1},
    {"-----BEGIN RSA PRIVATE KEY-----", "-----END RSA PRIVATE KEY-----", 1, 1},
    {"-----BEGIN EC PRIVATE KEY-----", "-----END EC PRIVATE KEY-----", 2, 1}
  };
  const uint8_t *bytes = pem, *start = NULL, *end;
  uint8_t *der = NULL;
  BASE64_CTX ctx;
  der_reader input;
  size_t f, selected = 0, remaining;
  int n = 0, tail = 0, status = SALTS_CRYPTO_EINVAL;
  if (!output) return status;
  *output = NULL;
  if (!pem || !size || size > INT_MAX || size > SIZE_MAX - 80) return status;
  for (f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
    const uint8_t *candidate;
    if (formats[f].private_key != !!private_key) continue;
    candidate = find_marker(bytes, size, formats[f].begin);
    if (candidate && (!start || candidate < start)) { start = candidate; selected = f; }
  }
  if (!start) return status;
  start += strlen(formats[selected].begin);
  remaining = size - (size_t)(start - bytes);
  end = find_marker(start, remaining, formats[selected].end);
  if (!end) return status;
  der = malloc(size + 80);
  if (!der) return SALTS_CRYPTO_ENOMEM;
  base64_decode_init(&ctx);
  if (base64_decode_update_ex(&ctx, start, (int)(end - start), der, &n, size + 80) < 0 || n < 0 ||
      base64_decode_finish_ex(&ctx, der + n, &tail, size + 80 - (size_t)n) != 1 || tail < 0) goto done;
  input.data = der; input.size = (size_t)n + (size_t)tail;
  if (formats[selected].type == 1) status = rsa_der(input, 1, output);
  else if (formats[selected].type == 2) status = ec_private_der(input, 0, output);
  else status = key_info_der(input, private_key, output);
done:
  cjwt_crypto_clear(der, size + 80); free(der);
  cjwt_crypto_clear(&ctx, sizeof(ctx));
  return status;
}
