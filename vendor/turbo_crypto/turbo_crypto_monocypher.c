#include "turbo_crypto.h"

#include "monocypher/monocypher.h"

static int valid_buffer(const void *buffer, size_t len) { return buffer != NULL || len == 0U; }

int turbo_crypto_blake2b(void *out, size_t out_len, const void *data, size_t data_len) {
  if (!out || out_len == 0U || out_len > TURBO_CRYPTO_BLAKE2B_MAX_SIZE ||
      !valid_buffer(data, data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  crypto_blake2b((uint8_t *)out, out_len, (const uint8_t *)data, data_len);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_blake2b_keyed(void *out, size_t out_len, const void *key, size_t key_len,
                               const void *data, size_t data_len) {
  if (!out || out_len == 0U || out_len > TURBO_CRYPTO_BLAKE2B_MAX_SIZE ||
      key_len > TURBO_CRYPTO_BLAKE2B_MAX_SIZE || !valid_buffer(key, key_len) ||
      !valid_buffer(data, data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  crypto_blake2b_keyed((uint8_t *)out, out_len, (const uint8_t *)key, key_len,
                       (const uint8_t *)data, data_len);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_aead_lock(void *cipher_text, uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE],
                           const uint8_t key[TURBO_CRYPTO_AEAD_KEY_SIZE],
                           const uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE],
                           const void *associated_data, size_t associated_data_len,
                           const void *plain_text, size_t text_len) {
  if (!mac || !key || !nonce || !valid_buffer(cipher_text, text_len) ||
      !valid_buffer(plain_text, text_len) || !valid_buffer(associated_data, associated_data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  crypto_aead_lock((uint8_t *)cipher_text, mac, key, nonce, (const uint8_t *)associated_data,
                   associated_data_len, (const uint8_t *)plain_text, text_len);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_aead_unlock(void *plain_text, const uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE],
                             const uint8_t key[TURBO_CRYPTO_AEAD_KEY_SIZE],
                             const uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE],
                             const void *associated_data, size_t associated_data_len,
                             const void *cipher_text, size_t text_len) {
  if (!mac || !key || !nonce || !valid_buffer(plain_text, text_len) ||
      !valid_buffer(cipher_text, text_len) || !valid_buffer(associated_data, associated_data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  return crypto_aead_unlock((uint8_t *)plain_text, mac, key, nonce,
                            (const uint8_t *)associated_data, associated_data_len,
                            (const uint8_t *)cipher_text, text_len) == 0
             ? TURBO_CRYPTO_OK
             : TURBO_CRYPTO_EVERIFY;
}

int turbo_crypto_argon2(void *out, uint32_t out_len, void *work_area,
                        turbo_crypto_argon2_config_t config, turbo_crypto_argon2_inputs_t inputs,
                        turbo_crypto_argon2_extras_t extras) {
  if (!out || out_len == 0U || !work_area || config.algorithm > TURBO_CRYPTO_ARGON2_ID ||
      config.lane_count == 0U || config.block_count / 8U < config.lane_count ||
      config.pass_count == 0U || !valid_buffer(inputs.password, inputs.password_size) ||
      !valid_buffer(inputs.salt, inputs.salt_size) || !valid_buffer(extras.key, extras.key_size) ||
      !valid_buffer(extras.associated_data, extras.associated_data_size)) {
    return TURBO_CRYPTO_EINVAL;
  }

  crypto_argon2_config native_config = {
      .algorithm = config.algorithm,
      .nb_blocks = config.block_count,
      .nb_passes = config.pass_count,
      .nb_lanes = config.lane_count,
  };
  crypto_argon2_inputs native_inputs = {
      .pass = inputs.password,
      .salt = inputs.salt,
      .pass_size = inputs.password_size,
      .salt_size = inputs.salt_size,
  };
  crypto_argon2_extras native_extras = {
      .key = extras.key,
      .ad = extras.associated_data,
      .key_size = extras.key_size,
      .ad_size = extras.associated_data_size,
  };
  crypto_argon2((uint8_t *)out, out_len, work_area, native_config, native_inputs, native_extras);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519_public_key(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!public_key || !secret_key) return TURBO_CRYPTO_EINVAL;
  crypto_x25519_public_key(public_key, secret_key);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519(uint8_t shared_secret[TURBO_CRYPTO_CURVE25519_SIZE],
                        const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE],
                        const uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!shared_secret || !secret_key || !public_key) return TURBO_CRYPTO_EINVAL;
  crypto_x25519(shared_secret, secret_key, public_key);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519_to_eddsa(uint8_t eddsa[TURBO_CRYPTO_CURVE25519_SIZE],
                                 const uint8_t x25519[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!eddsa || !x25519) return TURBO_CRYPTO_EINVAL;
  crypto_x25519_to_eddsa(eddsa, x25519);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519_inverse(uint8_t blind_salt[TURBO_CRYPTO_CURVE25519_SIZE],
                                const uint8_t private_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                const uint8_t curve_point[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!blind_salt || !private_key || !curve_point) return TURBO_CRYPTO_EINVAL;
  crypto_x25519_inverse(blind_salt, private_key, curve_point);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519_dirty_small(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                    const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!public_key || !secret_key) return TURBO_CRYPTO_EINVAL;
  crypto_x25519_dirty_small(public_key, secret_key);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_x25519_dirty_fast(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!public_key || !secret_key) return TURBO_CRYPTO_EINVAL;
  crypto_x25519_dirty_fast(public_key, secret_key);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_key_pair(uint8_t secret_key[TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE],
                                uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                uint8_t seed[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!secret_key || !public_key || !seed) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_key_pair(secret_key, public_key, seed);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_sign(uint8_t signature[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE],
                            const uint8_t secret_key[TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE],
                            const void *data, size_t data_len) {
  if (!signature || !secret_key || !valid_buffer(data, data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  crypto_eddsa_sign(signature, secret_key, (const uint8_t *)data, data_len);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_check(const uint8_t signature[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE],
                             const uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                             const void *data, size_t data_len) {
  if (!signature || !public_key || !valid_buffer(data, data_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  return crypto_eddsa_check(signature, public_key, (const uint8_t *)data, data_len) == 0
             ? TURBO_CRYPTO_OK
             : TURBO_CRYPTO_EVERIFY;
}

int turbo_crypto_eddsa_to_x25519(uint8_t x25519[TURBO_CRYPTO_CURVE25519_SIZE],
                                 const uint8_t eddsa[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!x25519 || !eddsa) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_to_x25519(x25519, eddsa);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_trim_scalar(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t in[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!out || !in) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_trim_scalar(out, in);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_reduce(uint8_t reduced[TURBO_CRYPTO_CURVE25519_SIZE],
                              const uint8_t expanded[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE]) {
  if (!reduced || !expanded) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_reduce(reduced, expanded);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_mul_add(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t a[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t b[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t c[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!out || !a || !b || !c) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_mul_add(out, a, b, c);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_eddsa_scalarbase(uint8_t point[TURBO_CRYPTO_CURVE25519_SIZE],
                                  const uint8_t scalar[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!point || !scalar) return TURBO_CRYPTO_EINVAL;
  crypto_eddsa_scalarbase(point, scalar);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_chacha20_h(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t in[TURBO_CRYPTO_CHACHA20_H_INPUT_SIZE]) {
  if (!out || !key || !in) return TURBO_CRYPTO_EINVAL;
  crypto_chacha20_h(out, key, in);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_chacha20_djb(void *cipher_text, const void *plain_text, size_t text_len,
                              const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                              const uint8_t nonce[TURBO_CRYPTO_CHACHA20_DJB_NONCE_SIZE],
                              uint64_t counter) {
  if (!key || !nonce || !valid_buffer(cipher_text, text_len) ||
      !valid_buffer(plain_text, text_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  (void)crypto_chacha20_djb((uint8_t *)cipher_text, (const uint8_t *)plain_text, text_len, key,
                            nonce, counter);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_chacha20_ietf(void *cipher_text, const void *plain_text, size_t text_len,
                               const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t nonce[TURBO_CRYPTO_CHACHA20_IETF_NONCE_SIZE],
                               uint32_t counter) {
  if (!key || !nonce || !valid_buffer(cipher_text, text_len) ||
      !valid_buffer(plain_text, text_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  (void)crypto_chacha20_ietf((uint8_t *)cipher_text, (const uint8_t *)plain_text, text_len, key,
                             nonce, counter);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_chacha20_x(void *cipher_text, const void *plain_text, size_t text_len,
                            const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t nonce[TURBO_CRYPTO_CHACHA20_X_NONCE_SIZE],
                            uint64_t counter) {
  if (!key || !nonce || !valid_buffer(cipher_text, text_len) ||
      !valid_buffer(plain_text, text_len)) {
    return TURBO_CRYPTO_EINVAL;
  }
  (void)crypto_chacha20_x((uint8_t *)cipher_text, (const uint8_t *)plain_text, text_len, key, nonce,
                          counter);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_poly1305(uint8_t mac[TURBO_CRYPTO_POLY1305_MAC_SIZE], const void *data,
                          size_t data_len, const uint8_t key[TURBO_CRYPTO_POLY1305_KEY_SIZE]) {
  if (!mac || !key || !valid_buffer(data, data_len)) return TURBO_CRYPTO_EINVAL;
  crypto_poly1305(mac, (const uint8_t *)data, data_len, key);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_elligator_map(uint8_t curve[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!curve || !hidden) return TURBO_CRYPTO_EINVAL;
  crypto_elligator_map(curve, hidden);
  return TURBO_CRYPTO_OK;
}

int turbo_crypto_elligator_rev(uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t curve[TURBO_CRYPTO_CURVE25519_SIZE], uint8_t tweak) {
  if (!hidden || !curve) return TURBO_CRYPTO_EINVAL;
  return crypto_elligator_rev(hidden, curve, tweak) == 0 ? TURBO_CRYPTO_OK : TURBO_CRYPTO_ECRYPTO;
}

int turbo_crypto_elligator_key_pair(uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE],
                                    uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                    uint8_t seed[TURBO_CRYPTO_CURVE25519_SIZE]) {
  if (!hidden || !secret_key || !seed) return TURBO_CRYPTO_EINVAL;
  crypto_elligator_key_pair(hidden, secret_key, seed);
  return TURBO_CRYPTO_OK;
}
