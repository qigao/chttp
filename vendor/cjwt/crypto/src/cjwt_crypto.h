#ifndef CHTTP_CJWT_CRYPTO_H
#define CHTTP_CJWT_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE 57U
#define SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE 57U
#define SALTS_CRYPTO_ED448_SIGNATURE_SIZE 114U
#define SALTS_CRYPTO_ED25519_PRIVATE_KEY_SIZE 32U
#define SALTS_CRYPTO_ED25519_PUBLIC_KEY_SIZE 32U
#define SALTS_CRYPTO_ED25519_SIGNATURE_SIZE 64U

typedef enum cjwt_crypto_status {
  SALTS_CRYPTO_OK = 0,
  SALTS_CRYPTO_EINVAL = -1,
  SALTS_CRYPTO_EVERIFY = -2,
  SALTS_CRYPTO_ERANDOM = -3,
  SALTS_CRYPTO_ECRYPTO = -4,
  SALTS_CRYPTO_ESTATE = -5,
  SALTS_CRYPTO_ENOMEM = -6
} cjwt_crypto_status;

typedef enum cjwt_crypto_hash {
  SALTS_CRYPTO_SHA1 = 1,
  SALTS_CRYPTO_SHA256 = 2,
  SALTS_CRYPTO_SHA384 = 3,
  SALTS_CRYPTO_SHA512 = 4
} cjwt_crypto_hash;

/** Caller-owned buffers; NULL inputs are allowed only with zero length.
 * Digest/HMAC output_size must be disjoint from all buffers. They set it to
 * zero on failure. EINVAL preserves output; provider failure returns ECRYPTO.
 * The digest/HMAC output may overlap inputs; PBKDF2 output must be disjoint.
 * SHA-1 exists for protocol interoperability. New signatures should use SHA-2.
 * Example: cjwt_crypto_digest(SALTS_CRYPTO_SHA384, data, size, out, 48, &n).
 */
int cjwt_crypto_digest(cjwt_crypto_hash hash, const void *data, size_t size,
    uint8_t *output, size_t capacity, size_t *output_size);
int cjwt_crypto_hmac(cjwt_crypto_hash hash, const void *key, size_t key_size,
    const void *data, size_t size, uint8_t *output, size_t capacity, size_t *output_size);
int cjwt_crypto_pbkdf2(cjwt_crypto_hash hash, const void *password, size_t password_size,
    const void *salt, size_t salt_size, uint32_t iterations, uint8_t *output, size_t size);
/** CSPRNG; ERANDOM clears the requested output range. Zero length is valid. */
int cjwt_crypto_random(void *output, size_t size);
/** Wipes caller-owned memory. NULL is valid only for size zero. */
void cjwt_crypto_clear(void *data, size_t size);
/** Constant-time comparison for equal public lengths; OK/EVERIFY/EINVAL. */
int cjwt_crypto_equal(const void *a, const void *b, size_t size);

/** AES-GCM: keys 16/24/32 bytes; nonempty IV; tags 1..16 bytes. All buffers
 * belong to the caller. input/output may be identical or disjoint; other
 * ranges must be disjoint. Capacity must cover size. EINVAL leaves output
 * unchanged. Authentication failure returns EVERIFY without plaintext output.
 * GCM nonce uniqueness and tag length selection belong to the protocol caller.
 * Example: use a fresh 12-byte IV and 16-byte tag for each JWE encryption.
 */
int cjwt_crypto_aes_gcm_encrypt(const uint8_t *key, size_t key_size,
    const uint8_t *iv, size_t iv_size, const void *aad, size_t aad_size,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity,
    uint8_t *tag, size_t tag_size);
int cjwt_crypto_aes_gcm_decrypt(const uint8_t *key, size_t key_size,
    const uint8_t *iv, size_t iv_size, const void *aad, size_t aad_size,
    const uint8_t *input, size_t size, const uint8_t *tag, size_t tag_size,
    uint8_t *output, size_t capacity);

/**
 * RFC 3394 AES Key Wrap using a 16/24/32-byte key-encryption key.
 * Input is at least 16 bytes and a multiple of 8; output requires size + 8.
 * Buffers belong to the caller and may overlap. output_size must not overlap
 * any buffer. Returns OK or EINVAL; *output_size is zero on failure.
 */
int cjwt_crypto_aes_key_wrap(const uint8_t *key, size_t key_size,
                              const uint8_t *input, size_t input_size,
                              uint8_t *output, size_t capacity, size_t *output_size);

/**
 * Unwraps RFC 3394 ciphertext (at least 24 bytes, a multiple of 8).
 * Output requires input_size - 8 bytes. Same ownership/alias contract as wrap.
 * Invalid arguments return EINVAL without changing output. Integrity failure
 * returns EVERIFY, clears the candidate plaintext, and sets *output_size to 0.
 */
int cjwt_crypto_aes_key_unwrap(const uint8_t *key, size_t key_size,
                                const uint8_t *input, size_t input_size,
                                uint8_t *output, size_t capacity, size_t *output_size);

/**
 * Derives an RFC 8032 Ed448 public key from a 57-byte private seed.
 *
 * @return SALTS_CRYPTO_OK, SALTS_CRYPTO_EINVAL for invalid arguments, or
 *         SALTS_CRYPTO_ECRYPTO when key derivation fails.
 */
int cjwt_crypto_ed448_public_key(const uint8_t private_key[SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE],
                                  uint8_t public_key[SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Creates a random RFC 8032 Ed448 key pair using the platform CSPRNG.
 * The two output ranges must not overlap.
 */
int cjwt_crypto_ed448_keygen(uint8_t private_key[SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE],
                              uint8_t public_key[SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE]);

/**
 * Signs data with RFC 8032 pure Ed448. A null data pointer is valid only when
 * data_size is zero.
 */
int cjwt_crypto_ed448_sign(const uint8_t private_key[SALTS_CRYPTO_ED448_PRIVATE_KEY_SIZE],
                            const void *data, size_t data_size,
                            uint8_t signature[SALTS_CRYPTO_ED448_SIGNATURE_SIZE]);

/**
 * Verifies an RFC 8032 pure Ed448 signature.
 *
 * @return SALTS_CRYPTO_OK for a valid signature, SALTS_CRYPTO_EVERIFY for an
 *         invalid key or signature, or another cjwt_crypto_status on failure.
 */
int cjwt_crypto_ed448_verify(const uint8_t public_key[SALTS_CRYPTO_ED448_PUBLIC_KEY_SIZE],
                              const void *data, size_t data_size,
                              const uint8_t signature[SALTS_CRYPTO_ED448_SIGNATURE_SIZE]);

/**
 * RFC 8032 pure Ed25519 with a 32-byte seed, 32-byte encoded public key and
 * 64-byte signature. The ownership, overlap, empty-message and status contracts
 * are identical to the corresponding Ed448 operations above.
 */
int cjwt_crypto_ed25519_public_key(const uint8_t private_key[32], uint8_t public_key[32]);
int cjwt_crypto_ed25519_keygen(uint8_t private_key[32], uint8_t public_key[32]);
int cjwt_crypto_ed25519_sign(const uint8_t private_key[32], const void *data,
                              size_t data_size, uint8_t signature[64]);
int cjwt_crypto_ed25519_verify(const uint8_t public_key[32], const void *data,
                                size_t data_size, const uint8_t signature[64]);

typedef enum cjwt_crypto_ec_curve {
  SALTS_CRYPTO_EC_P256 = 1,
  SALTS_CRYPTO_EC_P384 = 2,
  SALTS_CRYPTO_EC_P521 = 3,
  SALTS_CRYPTO_EC_SECP256K1 = 4
} cjwt_crypto_ec_curve;

/**
 * ECDSA scalar/coordinate sizes are 32 (P256/k1), 48 (P384), 66 (P521).
 * Private keys are fixed-width big-endian scalars; public keys are X || Y,
 * signatures R || S (twice the scalar size), without SEC1/DER framing.
 * Curve/hash pairs are P256/k1 SHA-256, P384 SHA-384, P521 SHA-512.
 * All buffers are caller-owned; input/output may overlap. On a provider failure
 * output is cleared. Invalid arguments return EINVAL without changing output.
 * NULL data is allowed only for size zero; data_size must fit uint32_t.
 */
int cjwt_crypto_ecdsa_public_key(cjwt_crypto_ec_curve curve,
    const uint8_t *private_key, size_t private_size, uint8_t *public_key, size_t public_size);
int cjwt_crypto_ecdsa_sign(cjwt_crypto_ec_curve curve,
    const uint8_t *private_key, size_t private_size, const void *data, size_t data_size,
    uint8_t *signature, size_t signature_size);
/** Returns EVERIFY for invalid points/signatures, EINVAL for invalid sizes. */
int cjwt_crypto_ecdsa_verify(cjwt_crypto_ec_curve curve,
    const uint8_t *public_key, size_t public_size, const void *data, size_t data_size,
    const uint8_t *signature, size_t signature_size);

typedef struct cjwt_crypto_key cjwt_crypto_key;
typedef struct cjwt_crypto_bytes { const uint8_t *data; size_t size; } cjwt_crypto_bytes;
typedef enum cjwt_crypto_key_kind {
  SALTS_CRYPTO_KEY_RSA = 1, SALTS_CRYPTO_KEY_EC,
  SALTS_CRYPTO_KEY_ED25519, SALTS_CRYPTO_KEY_ED448
} cjwt_crypto_key_kind;
typedef enum cjwt_crypto_signature {
  SALTS_CRYPTO_RSA_PKCS1 = 1, SALTS_CRYPTO_RSA_PSS,
  SALTS_CRYPTO_ECDSA, SALTS_CRYPTO_EDDSA
} cjwt_crypto_signature;

/** Imports copy all input bytes. On success *output is an immutable owned key;
 * on failure it is NULL (EINVAL, ENOMEM, ERANDOM or ECRYPTO). The caller destroys
 * it after all synchronous/concurrent readers return. Destruction wipes secrets.
 * RSA accepts n/e and optional d; CRT values, if complete, are consistency-checked.
 * EC public input is SEC1 compressed/uncompressed, private input a scalar.
 * EdDSA private input is a seed. Supplied public/private components must agree.
 * PEM supports SPKI public keys, PKCS8 and traditional RSA/EC private keys.
 * No passphrase callback or encrypted-key import is provided.
 * Example: cjwt_crypto_key_from_pem(pem, length, 1, &key); ...;
 *          cjwt_crypto_key_destroy(key);
 */
int cjwt_crypto_key_from_rsa(cjwt_crypto_bytes n, cjwt_crypto_bytes e, cjwt_crypto_bytes d,
    cjwt_crypto_bytes p, cjwt_crypto_bytes q, cjwt_crypto_bytes dp,
    cjwt_crypto_bytes dq, cjwt_crypto_bytes qi, cjwt_crypto_key **output);
int cjwt_crypto_key_from_ec(cjwt_crypto_ec_curve curve, cjwt_crypto_bytes public_key,
    cjwt_crypto_bytes private_key, cjwt_crypto_key **output);
int cjwt_crypto_key_from_ed(cjwt_crypto_key_kind kind, cjwt_crypto_bytes public_key,
    cjwt_crypto_bytes private_key, cjwt_crypto_key **output);
int cjwt_crypto_key_from_pem(const void *pem, size_t size, int private_key, cjwt_crypto_key **output);
void cjwt_crypto_key_destroy(cjwt_crypto_key *key);
cjwt_crypto_key_kind cjwt_crypto_key_type(const cjwt_crypto_key *key);
size_t cjwt_crypto_key_signature_size(const cjwt_crypto_key *key);
/** Message signatures, caller-owned output. Capacity is at least signature_size.
 * PSS signing uses digest-length salt; verification accepts any encoded salt.
 * RSA hashes SHA256/384/512; ECDSA uses the selected hash with the imported curve.
 * EdDSA ignores hash. Failure leaves *size zero; invalid signatures return EVERIFY.
 */
int cjwt_crypto_key_sign(const cjwt_crypto_key *key, cjwt_crypto_signature scheme,
    cjwt_crypto_hash hash, const void *message, size_t message_size,
    uint8_t *signature, size_t capacity, size_t *size);
int cjwt_crypto_key_verify(const cjwt_crypto_key *key, cjwt_crypto_signature scheme,
    cjwt_crypto_hash hash, const void *message, size_t message_size,
    const uint8_t *signature, size_t size);
/** RSA OAEP with empty label, MGF1 uses hash. Caller owns disjoint input/output
 * and output_size. Decrypt exposes no plaintext on failure (EVERIFY); key/size
 * errors return EINVAL. Encrypt output needs key_signature_size bytes.
 */
int cjwt_crypto_key_oaep_encrypt(const cjwt_crypto_key *key, cjwt_crypto_hash hash,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *output_size);
int cjwt_crypto_key_oaep_decrypt(const cjwt_crypto_key *key, cjwt_crypto_hash hash,
    const uint8_t *input, size_t size, uint8_t *output, size_t capacity, size_t *output_size);

#ifdef __cplusplus
}
#endif

#endif /* CHTTP_CJWT_CRYPTO_H */
