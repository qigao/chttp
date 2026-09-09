#ifndef TURBO_CRYPTO_H
#define TURBO_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vendored implementations are private; this header is the library boundary. */

#define TURBO_CRYPTO_MD5_SIZE 16U
#define TURBO_CRYPTO_SHA256_SIZE 32U
#define TURBO_CRYPTO_SHA256_BLOCK_SIZE 64U
#define TURBO_CRYPTO_SHA256_CONTEXT_SIZE 192U
#define TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE 57U
#define TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE 57U
#define TURBO_CRYPTO_ED448_SIGNATURE_SIZE 114U
#define TURBO_CRYPTO_BLAKE2B_MAX_SIZE 64U
#define TURBO_CRYPTO_AEAD_MAC_SIZE 16U
#define TURBO_CRYPTO_AEAD_KEY_SIZE 32U
#define TURBO_CRYPTO_AEAD_NONCE_SIZE 24U
#define TURBO_CRYPTO_CURVE25519_SIZE 32U
#define TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE 64U
#define TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE 64U
#define TURBO_CRYPTO_CHACHA20_H_INPUT_SIZE 16U
#define TURBO_CRYPTO_CHACHA20_DJB_NONCE_SIZE 8U
#define TURBO_CRYPTO_CHACHA20_IETF_NONCE_SIZE 12U
#define TURBO_CRYPTO_CHACHA20_X_NONCE_SIZE 24U
#define TURBO_CRYPTO_POLY1305_MAC_SIZE 16U
#define TURBO_CRYPTO_POLY1305_KEY_SIZE 32U
#define TURBO_CRYPTO_XXTEA_KEY_SIZE 16U

#define TURBO_CRYPTO_ARGON2_D 0U
#define TURBO_CRYPTO_ARGON2_I 1U
#define TURBO_CRYPTO_ARGON2_ID 2U

#define TURBO_CRYPTO_OK 0
#define TURBO_CRYPTO_EINVAL (-1)
#define TURBO_CRYPTO_ESTATE (-2)
#define TURBO_CRYPTO_EVERIFY (-3)
#define TURBO_CRYPTO_ERANDOM (-4)
#define TURBO_CRYPTO_ECRYPTO (-5)
#define TURBO_CRYPTO_EBUFFER (-6)

/**
 * SHA-256 streaming context with implementation-private storage.
 *
 * The context has no heap ownership and is not thread-safe. It must not be
 * copied after initialization because the private state contains self
 * references. One owner must call init, zero or more updates, then final.
 */
typedef union turbo_crypto_sha256_ctx_u {
    void* pointer_alignment;
    uint64_t integer_alignment;
    long double floating_alignment;
    uint8_t bytes[TURBO_CRYPTO_SHA256_CONTEXT_SIZE];
} turbo_crypto_sha256_ctx_t;

typedef struct turbo_crypto_argon2_config_s {
  uint32_t algorithm;
  uint32_t block_count;
  uint32_t pass_count;
  uint32_t lane_count;
} turbo_crypto_argon2_config_t;

typedef struct turbo_crypto_argon2_inputs_s {
  const uint8_t *password;
  const uint8_t *salt;
  uint32_t password_size;
  uint32_t salt_size;
} turbo_crypto_argon2_inputs_t;

typedef struct turbo_crypto_argon2_extras_s {
  const uint8_t *key;
  const uint8_t *associated_data;
  uint32_t key_size;
  uint32_t associated_data_size;
} turbo_crypto_argon2_extras_t;

/** Time O(len), space O(1). NULL data is valid only when len is zero. */
int turbo_crypto_sha256(const void* data, size_t len,
                        uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);
int turbo_crypto_sha256_init(turbo_crypto_sha256_ctx_t* ctx);
int turbo_crypto_sha256_update(turbo_crypto_sha256_ctx_t* ctx,
                               const void* data, size_t len);
int turbo_crypto_sha256_final(turbo_crypto_sha256_ctx_t* ctx,
                              uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);

/** RFC 2104 HMAC-SHA256. Time O(key_len + data_len), space O(1). */
int turbo_crypto_hmac_sha256(const void* key, size_t key_len,
                             const void* data, size_t data_len,
                             uint8_t out[TURBO_CRYPTO_SHA256_SIZE]);

/** RFC 8018 PBKDF2-HMAC-SHA256. Iterations must be nonzero. */
int turbo_crypto_pbkdf2_hmac_sha256(
    const void* password, size_t password_len,
    const void* salt, size_t salt_len,
    uint32_t iterations, void* out, size_t out_len);

/**
 * RFC 1321 MD5 for protocols that explicitly require MD5.
 * MD5 must not be used for passwords, signatures, or new security designs.
 * Time O(len), space O(1).
 */
int turbo_crypto_md5(const void* data, size_t len,
                     uint8_t out[TURBO_CRYPTO_MD5_SIZE]);

/**
 * Calculate the ciphertext size produced by turbo_crypto_xxtea_encrypt().
 *
 * XXTEA is exposed only for compatibility with existing data formats. It is
 * deterministic and unauthenticated; new protocols should use
 * turbo_crypto_aead_lock() instead.
 *
 * @param plain_text_len Plaintext length in bytes; zero is not supported.
 * @param cipher_text_len Receives the exact required output size.
 * @return TURBO_CRYPTO_OK or TURBO_CRYPTO_EINVAL for an invalid or
 *         unrepresentable length.
 */
int turbo_crypto_xxtea_encrypt_size(size_t plain_text_len,
                                    size_t* cipher_text_len);

/**
 * Encrypt bytes with the xxtea-c compatible data framing.
 *
 * Keys shorter than 16 bytes are zero-padded. For compatibility with
 * xxtea-c, the first zero byte terminates the effective key. Input and output
 * may overlap. Time O(plain_text_len), temporary space O(plain_text_len).
 *
 * @param cipher_text Caller-owned output buffer.
 * @param cipher_text_capacity Available bytes in cipher_text.
 * @param cipher_text_len Receives the exact ciphertext size, including when
 *        TURBO_CRYPTO_EBUFFER is returned.
 * @param plain_text Non-empty plaintext bytes.
 * @param plain_text_len Plaintext length in bytes.
 * @param key Key bytes; NULL is valid only when key_len is zero.
 * @param key_len Key length from zero through TURBO_CRYPTO_XXTEA_KEY_SIZE.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid arguments,
 *         TURBO_CRYPTO_EBUFFER for insufficient output capacity, or
 *         TURBO_CRYPTO_ECRYPTO when the private implementation fails.
 */
int turbo_crypto_xxtea_encrypt(void* cipher_text,
                               size_t cipher_text_capacity,
                               size_t* cipher_text_len,
                               const void* plain_text,
                               size_t plain_text_len,
                               const void* key,
                               size_t key_len);

/**
 * Decrypt bytes produced by turbo_crypto_xxtea_encrypt() or xxtea-c.
 *
 * The output buffer must have at least cipher_text_len - 4 bytes available;
 * plain_text_len receives the actual plaintext size. Because XXTEA has no
 * authentication, successful decryption does not prove ciphertext integrity.
 * Input and output may overlap. Time O(cipher_text_len), temporary space
 * O(cipher_text_len).
 *
 * @param plain_text Caller-owned output buffer.
 * @param plain_text_capacity Available bytes in plain_text.
 * @param plain_text_len Receives the actual plaintext size on success and the
 *        required upper bound when TURBO_CRYPTO_EBUFFER is returned.
 * @param cipher_text Ciphertext bytes with xxtea-c length framing.
 * @param cipher_text_len Ciphertext length; at least eight and divisible by
 *        four.
 * @param key Key bytes; NULL is valid only when key_len is zero.
 * @param key_len Key length from zero through TURBO_CRYPTO_XXTEA_KEY_SIZE.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid arguments or
 *         framing, TURBO_CRYPTO_EBUFFER for insufficient output capacity, or
 *         TURBO_CRYPTO_ECRYPTO when decryption fails.
 */
int turbo_crypto_xxtea_decrypt(void* plain_text,
                               size_t plain_text_capacity,
                               size_t* plain_text_len,
                               const void* cipher_text,
                               size_t cipher_text_len,
                               const void* key,
                               size_t key_len);

/**
 * Derive an RFC 8032 Ed448 public key from a 57-byte private seed.
 *
 * @param private_key Input seed owned by the caller and left unchanged.
 * @param public_key Output public key owned by the caller.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for NULL arguments, or
 *         TURBO_CRYPTO_ECRYPTO when key derivation fails.
 */
int turbo_crypto_ed448_public_key(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Generate an RFC 8032 Ed448 private seed and its public key.
 *
 * The output buffers must be distinct. The caller owns both buffers and must
 * erase the private seed when it is no longer needed.
 *
 * @param private_key Output private seed.
 * @param public_key Output public key.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid buffers,
 *         TURBO_CRYPTO_ERANDOM when the CSPRNG fails, or
 *         TURBO_CRYPTO_ECRYPTO when key derivation fails.
 */
int turbo_crypto_ed448_keygen(
    uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Create a deterministic pure Ed448 signature as defined by RFC 8032.
 *
 * NULL data is valid only when data_len is zero. Message lengths greater than
 * UINT32_MAX are rejected because libecc's streaming boundary uses u32.
 *
 * @param private_key Input 57-byte private seed.
 * @param data Message bytes.
 * @param data_len Message length in bytes.
 * @param signature Output 114-byte signature.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid input, or
 *         TURBO_CRYPTO_ECRYPTO when signing fails.
 */
int turbo_crypto_ed448_sign(
    const uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE],
    const void* data, size_t data_len,
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]);

/**
 * Verify a pure RFC 8032 Ed448 signature.
 *
 * @param public_key Input 57-byte canonical public key.
 * @param data Message bytes; NULL is valid only when data_len is zero.
 * @param data_len Message length in bytes, at most UINT32_MAX.
 * @param signature Input 114-byte signature.
 * @return TURBO_CRYPTO_OK, TURBO_CRYPTO_EINVAL for invalid arguments, or
 *         TURBO_CRYPTO_EVERIFY for an invalid key or signature. An internal
 *         curve initialization failure returns TURBO_CRYPTO_ECRYPTO.
 */
int turbo_crypto_ed448_verify(
    const uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE],
    const void* data, size_t data_len,
    const uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE]);

/**
 * One-shot Monocypher-backed primitives. Input pointers may be NULL only when
 * their corresponding size is zero. Output, key, nonce, and fixed-size input
 * pointers are always required. Functions return TURBO_CRYPTO_OK,
 * TURBO_CRYPTO_EINVAL, TURBO_CRYPTO_EVERIFY for authentication/signature
 * failures, or TURBO_CRYPTO_ECRYPTO when a mapping operation cannot complete.
 */
int turbo_crypto_blake2b(void *out, size_t out_len, const void *data, size_t data_len);
int turbo_crypto_blake2b_keyed(void *out, size_t out_len, const void *key, size_t key_len,
                               const void *data, size_t data_len);

int turbo_crypto_aead_lock(void *cipher_text, uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE],
                           const uint8_t key[TURBO_CRYPTO_AEAD_KEY_SIZE],
                           const uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE],
                           const void *associated_data, size_t associated_data_len,
                           const void *plain_text, size_t text_len);
int turbo_crypto_aead_unlock(void *plain_text, const uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE],
                             const uint8_t key[TURBO_CRYPTO_AEAD_KEY_SIZE],
                             const uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE],
                             const void *associated_data, size_t associated_data_len,
                             const void *cipher_text, size_t text_len);

/**
 * Argon2 uses caller-owned work memory of block_count * 1024 bytes. The
 * wrapper is single-threaded and requires block_count >= 8 * lane_count.
 */
int turbo_crypto_argon2(void *out, uint32_t out_len, void *work_area,
                        turbo_crypto_argon2_config_t config, turbo_crypto_argon2_inputs_t inputs,
                        turbo_crypto_argon2_extras_t extras);

int turbo_crypto_x25519_public_key(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_x25519(uint8_t shared_secret[TURBO_CRYPTO_CURVE25519_SIZE],
                        const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE],
                        const uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_x25519_to_eddsa(uint8_t eddsa[TURBO_CRYPTO_CURVE25519_SIZE],
                                 const uint8_t x25519[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_x25519_inverse(uint8_t blind_salt[TURBO_CRYPTO_CURVE25519_SIZE],
                                const uint8_t private_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                const uint8_t curve_point[TURBO_CRYPTO_CURVE25519_SIZE]);

/** Dirty public keys are for Elligator use and leak three private-key bits. */
int turbo_crypto_x25519_dirty_small(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                    const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_x25519_dirty_fast(uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE]);

int turbo_crypto_eddsa_key_pair(uint8_t secret_key[TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE],
                                uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                uint8_t seed[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_eddsa_sign(uint8_t signature[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE],
                            const uint8_t secret_key[TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE],
                            const void *data, size_t data_len);
int turbo_crypto_eddsa_check(const uint8_t signature[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE],
                             const uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE],
                             const void *data, size_t data_len);
int turbo_crypto_eddsa_to_x25519(uint8_t x25519[TURBO_CRYPTO_CURVE25519_SIZE],
                                 const uint8_t eddsa[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_eddsa_trim_scalar(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                                   const uint8_t in[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_eddsa_reduce(uint8_t reduced[TURBO_CRYPTO_CURVE25519_SIZE],
                              const uint8_t expanded[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE]);
int turbo_crypto_eddsa_mul_add(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t a[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t b[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t c[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_eddsa_scalarbase(uint8_t point[TURBO_CRYPTO_CURVE25519_SIZE],
                                  const uint8_t scalar[TURBO_CRYPTO_CURVE25519_SIZE]);

int turbo_crypto_chacha20_h(uint8_t out[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t in[TURBO_CRYPTO_CHACHA20_H_INPUT_SIZE]);
int turbo_crypto_chacha20_djb(void *cipher_text, const void *plain_text, size_t text_len,
                              const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                              const uint8_t nonce[TURBO_CRYPTO_CHACHA20_DJB_NONCE_SIZE],
                              uint64_t counter);
int turbo_crypto_chacha20_ietf(void *cipher_text, const void *plain_text, size_t text_len,
                               const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t nonce[TURBO_CRYPTO_CHACHA20_IETF_NONCE_SIZE],
                               uint32_t counter);
int turbo_crypto_chacha20_x(void *cipher_text, const void *plain_text, size_t text_len,
                            const uint8_t key[TURBO_CRYPTO_CURVE25519_SIZE],
                            const uint8_t nonce[TURBO_CRYPTO_CHACHA20_X_NONCE_SIZE],
                            uint64_t counter);

int turbo_crypto_poly1305(uint8_t mac[TURBO_CRYPTO_POLY1305_MAC_SIZE], const void *data,
                          size_t data_len, const uint8_t key[TURBO_CRYPTO_POLY1305_KEY_SIZE]);
int turbo_crypto_elligator_map(uint8_t curve[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE]);
int turbo_crypto_elligator_rev(uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE],
                               const uint8_t curve[TURBO_CRYPTO_CURVE25519_SIZE], uint8_t tweak);
int turbo_crypto_elligator_key_pair(uint8_t hidden[TURBO_CRYPTO_CURVE25519_SIZE],
                                    uint8_t secret_key[TURBO_CRYPTO_CURVE25519_SIZE],
                                    uint8_t seed[TURBO_CRYPTO_CURVE25519_SIZE]);

/** Fill output from the operating-system CSPRNG. */
int turbo_crypto_random(void *out, size_t len);

/** Constant-time byte comparison. */
int turbo_crypto_verify(const void *expected, const void *actual, size_t len);

/** Erase sensitive memory through Monocypher. */
void turbo_crypto_wipe(void *secret, size_t len);

#ifdef __cplusplus
}
#endif

#endif
