#include "cjwt_crypto.h"
#include <gmssl/aes_key_wrap.h>

int cjwt_crypto_aes_key_wrap(const uint8_t *key, size_t key_size,
                              const uint8_t *input, size_t input_size,
                              uint8_t *output, size_t capacity, size_t *output_size) {
  return aes_key_wrap(key, key_size, input, input_size, output, capacity, output_size) == 1
             ? SALTS_CRYPTO_OK : SALTS_CRYPTO_EINVAL;
}

int cjwt_crypto_aes_key_unwrap(const uint8_t *key, size_t key_size,
                                const uint8_t *input, size_t input_size,
                                uint8_t *output, size_t capacity, size_t *output_size) {
  if (output_size) *output_size = 0;
  if (!key || !input || !output || !output_size ||
      (key_size != 16 && key_size != 24 && key_size != 32) ||
      input_size < 24 || input_size % 8 || capacity < input_size - 8 ||
      (input_size - 8) / 8 > UINT64_MAX / 6) return SALTS_CRYPTO_EINVAL;
  return aes_key_unwrap(key, key_size, input, input_size, output, capacity, output_size) == 1
             ? SALTS_CRYPTO_OK : SALTS_CRYPTO_EVERIFY;
}
