#include "tinytest.h"
#include "turbo_crypto.h"

#include <string.h>

static const uint8_t ed448_rfc8032_private_key[
    TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE] = {
    0x6c, 0x82, 0xa5, 0x62, 0xcb, 0x80, 0x8d, 0x10,
    0xd6, 0x32, 0xbe, 0x89, 0xc8, 0x51, 0x3e, 0xbf,
    0x6c, 0x92, 0x9f, 0x34, 0xdd, 0xfa, 0x8c, 0x9f,
    0x63, 0xc9, 0x96, 0x0e, 0xf6, 0xe3, 0x48, 0xa3,
    0x52, 0x8c, 0x8a, 0x3f, 0xcc, 0x2f, 0x04, 0x4e,
    0x39, 0xa3, 0xfc, 0x5b, 0x94, 0x49, 0x2f, 0x8f,
    0x03, 0x2e, 0x75, 0x49, 0xa2, 0x00, 0x98, 0xf9,
    0x5b};

static const uint8_t ed448_rfc8032_public_key[
    TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE] = {
    0x5f, 0xd7, 0x44, 0x9b, 0x59, 0xb4, 0x61, 0xfd,
    0x2c, 0xe7, 0x87, 0xec, 0x61, 0x6a, 0xd4, 0x6a,
    0x1d, 0xa1, 0x34, 0x24, 0x85, 0xa7, 0x0e, 0x1f,
    0x8a, 0x0e, 0xa7, 0x5d, 0x80, 0xe9, 0x67, 0x78,
    0xed, 0xf1, 0x24, 0x76, 0x9b, 0x46, 0xc7, 0x06,
    0x1b, 0xd6, 0x78, 0x3d, 0xf1, 0xe5, 0x0f, 0x6c,
    0xd1, 0xfa, 0x1a, 0xbe, 0xaf, 0xe8, 0x25, 0x61,
    0x80};

static const uint8_t ed448_rfc8032_signature[
    TURBO_CRYPTO_ED448_SIGNATURE_SIZE] = {
    0x53, 0x3a, 0x37, 0xf6, 0xbb, 0xe4, 0x57, 0x25,
    0x1f, 0x02, 0x3c, 0x0d, 0x88, 0xf9, 0x76, 0xae,
    0x2d, 0xfb, 0x50, 0x4a, 0x84, 0x3e, 0x34, 0xd2,
    0x07, 0x4f, 0xd8, 0x23, 0xd4, 0x1a, 0x59, 0x1f,
    0x2b, 0x23, 0x3f, 0x03, 0x4f, 0x62, 0x82, 0x81,
    0xf2, 0xfd, 0x7a, 0x22, 0xdd, 0xd4, 0x7d, 0x78,
    0x28, 0xc5, 0x9b, 0xd0, 0xa2, 0x1b, 0xfd, 0x39,
    0x80, 0xff, 0x0d, 0x20, 0x28, 0xd4, 0xb1, 0x8a,
    0x9d, 0xf6, 0x3e, 0x00, 0x6c, 0x5d, 0x1c, 0x2d,
    0x34, 0x5b, 0x92, 0x5d, 0x8d, 0xc0, 0x0b, 0x41,
    0x04, 0x85, 0x2d, 0xb9, 0x9a, 0xc5, 0xc7, 0xcd,
    0xda, 0x85, 0x30, 0xa1, 0x13, 0xa0, 0xf4, 0xdb,
    0xb6, 0x11, 0x49, 0xf0, 0x5a, 0x73, 0x63, 0x26,
    0x8c, 0x71, 0xd9, 0x58, 0x08, 0xff, 0x2e, 0x65,
    0x26, 0x00};

spec("turbo crypto") {
  it("matches the SHA-256 standard vector") {
    static const uint8_t expected[TURBO_CRYPTO_SHA256_SIZE] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t actual[TURBO_CRYPTO_SHA256_SIZE];

    check_equal(turbo_crypto_sha256("abc", 3, actual), TURBO_CRYPTO_OK);
    check_equal(actual, expected, sizeof(expected));
  }

  it("preserves SHA-256 state across incremental updates") {
    uint8_t expected[TURBO_CRYPTO_SHA256_SIZE];
    uint8_t actual[TURBO_CRYPTO_SHA256_SIZE];
    turbo_crypto_sha256_ctx_t ctx;

    check_equal(turbo_crypto_sha256("streamed content", 16, expected),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_init(&ctx), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_update(&ctx, "streamed ", 9),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_update(&ctx, "content", 7),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_final(&ctx, actual), TURBO_CRYPTO_OK);
    check_equal(actual, expected, sizeof(expected));
    check_equal(turbo_crypto_sha256_final(&ctx, actual), TURBO_CRYPTO_ESTATE);
  }

  it("matches RFC 4231 HMAC-SHA256 vectors including a long key") {
    static const uint8_t case1_expected[TURBO_CRYPTO_SHA256_SIZE] = {
        0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
        0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
        0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
        0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7
    };
    static const uint8_t case6_expected[TURBO_CRYPTO_SHA256_SIZE] = {
        0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f,
        0x0d, 0x8a, 0x26, 0xaa, 0xcb, 0xf5, 0xb7, 0x7f,
        0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14,
        0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54
    };
    static const char case6_data[] =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    uint8_t case1_key[20];
    uint8_t case6_key[131];
    uint8_t actual[TURBO_CRYPTO_SHA256_SIZE];
    memset(case1_key, 0x0b, sizeof(case1_key));
    memset(case6_key, 0xaa, sizeof(case6_key));

    check_equal(turbo_crypto_hmac_sha256(case1_key, sizeof(case1_key),
                                          "Hi There", 8, actual),
                 TURBO_CRYPTO_OK);
    check_equal(actual, case1_expected, sizeof(case1_expected));

    check_equal(turbo_crypto_hmac_sha256(case6_key, sizeof(case6_key),
                                          case6_data, sizeof(case6_data) - 1,
                                          actual),
                 TURBO_CRYPTO_OK);
    check_equal(actual, case6_expected, sizeof(case6_expected));
  }

  it("matches PBKDF2-HMAC-SHA256 vectors and supports multi-block output") {
    static const uint8_t iteration1_expected[TURBO_CRYPTO_SHA256_SIZE] = {
        0x12, 0x0f, 0xb6, 0xcf, 0xfc, 0xf8, 0xb3, 0x2c,
        0x43, 0xe7, 0x22, 0x52, 0x56, 0xc4, 0xf8, 0x37,
        0xa8, 0x65, 0x48, 0xc9, 0x2c, 0xcc, 0x35, 0x48,
        0x08, 0x05, 0x98, 0x7c, 0xb7, 0x0b, 0xe1, 0x7b
    };
    static const uint8_t iteration2_expected[40] = {
        0xae, 0x4d, 0x0c, 0x95, 0xaf, 0x6b, 0x46, 0xd3,
        0x2d, 0x0a, 0xdf, 0xf9, 0x28, 0xf0, 0x6d, 0xd0,
        0x2a, 0x30, 0x3f, 0x8e, 0xf3, 0xc2, 0x51, 0xdf,
        0xd6, 0xe2, 0xd8, 0x5a, 0x95, 0x47, 0x4c, 0x43,
        0x83, 0x06, 0x51, 0xaf, 0xcb, 0x5c, 0x86, 0x2f
    };
    uint8_t actual[40];

    check_equal(turbo_crypto_pbkdf2_hmac_sha256(
                     "password", 8, "salt", 4, 1, actual,
                     TURBO_CRYPTO_SHA256_SIZE),
                 TURBO_CRYPTO_OK);
    check_equal(actual, iteration1_expected,
                         sizeof(iteration1_expected));
    check_equal(turbo_crypto_pbkdf2_hmac_sha256(
                     "password", 8, "salt", 4, 2, actual, sizeof(actual)),
                 TURBO_CRYPTO_OK);
    check_equal(actual, iteration2_expected,
                         sizeof(iteration2_expected));
  }

  it("matches the RFC 1321 MD5 vectors") {
    static const struct {
      const char* input;
      const char* expected;
    } vectors[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
         "d174ab98d277d9f5a5611c2c9f419d9f"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "57edf4a22be3c955ac49da2e2107b67a"},
    };
    static const char hex_digits[] = "0123456789abcdef";
    uint8_t actual[TURBO_CRYPTO_MD5_SIZE];
    char actual_hex[TURBO_CRYPTO_MD5_SIZE * 2 + 1];

    for (size_t vector_index = 0;
         vector_index < sizeof(vectors) / sizeof(vectors[0]);
         ++vector_index) {
      check_equal(turbo_crypto_md5(vectors[vector_index].input,
                                    strlen(vectors[vector_index].input), actual),
                   TURBO_CRYPTO_OK);
      for (size_t i = 0; i < sizeof(actual); ++i) {
        actual_hex[i * 2] = hex_digits[(actual[i] >> 4) & 0x0f];
        actual_hex[i * 2 + 1] = hex_digits[actual[i] & 0x0f];
      }
      actual_hex[TURBO_CRYPTO_MD5_SIZE * 2] = '\0';
      check_equal(actual_hex, vectors[vector_index].expected);
    }
  }

  it("matches the official xxtea cross-language vector") {
    /* https://github.com/xxtea/xxtea-php/blob/master/tests/XXTEATest.php */
    static const uint8_t plain_text[] = {
        0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c,
        0x64, 0x21, 0x20, 0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd, 0xef,
        0xbc, 0x8c, 0xe4, 0xb8, 0xad, 0xe5, 0x9b, 0xbd, 0xf0, 0x9f,
        0x87, 0xa8, 0xf0, 0x9f, 0x87, 0xb3, 0xef, 0xbc, 0x81};
    static const uint8_t key[] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    static const uint8_t expected[] = {
        0x0f, 0x8b, 0x74, 0xad, 0x55, 0xd4, 0x0e, 0x5d, 0xdb, 0x9d,
        0x67, 0x44, 0x46, 0x1a, 0x89, 0x98, 0x52, 0x1a, 0x9d, 0xf9,
        0xff, 0xeb, 0x30, 0x31, 0x01, 0x8f, 0x63, 0x0f, 0xa9, 0xfd,
        0x31, 0x23, 0x10, 0x36, 0x80, 0xfc, 0x4c, 0xe4, 0xb8, 0xac,
        0x71, 0xdc, 0x1a, 0xe1};
    uint8_t cipher_text[sizeof(expected)];
    uint8_t decrypted[sizeof(expected) - 4U];
    size_t cipher_text_len = 0U;
    size_t plain_text_len = 0U;

    check_equal(turbo_crypto_xxtea_encrypt_size(sizeof(plain_text),
                                                  &cipher_text_len),
                 TURBO_CRYPTO_OK);
    check_equal(cipher_text_len, sizeof(expected));
    check_equal(turbo_crypto_xxtea_encrypt(
                     cipher_text, sizeof(cipher_text), &cipher_text_len,
                     plain_text, sizeof(plain_text), key, sizeof(key)),
                 TURBO_CRYPTO_OK);
    check_equal(cipher_text, expected, sizeof(expected));
    check_equal(turbo_crypto_xxtea_decrypt(
                     decrypted, sizeof(decrypted), &plain_text_len,
                     cipher_text, cipher_text_len, key, sizeof(key)),
                 TURBO_CRYPTO_OK);
    check_equal(plain_text_len, sizeof(plain_text));
    check_equal(decrypted, plain_text, sizeof(plain_text));
  }

  it("rejects invalid XXTEA framing and reports required capacity") {
    static const uint8_t plain_text[] = {0x00, 0x01, 0x00, 0xff, 0x7f};
    static const uint8_t key[] = {'l', 'e', 'g', 'a', 'c', 'y'};
    uint8_t cipher_text[12];
    uint8_t decrypted[8];
    size_t output_len = 0U;

    check_equal(turbo_crypto_xxtea_encrypt(
                     cipher_text, sizeof(cipher_text) - 1U, &output_len,
                     plain_text, sizeof(plain_text), key, sizeof(key)),
                 TURBO_CRYPTO_EBUFFER);
    check_equal(output_len, sizeof(cipher_text));
    check_equal(turbo_crypto_xxtea_encrypt(
                     cipher_text, sizeof(cipher_text), &output_len,
                     plain_text, sizeof(plain_text), key, sizeof(key)),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_xxtea_decrypt(
                     decrypted, sizeof(decrypted) - 1U, &output_len,
                     cipher_text, sizeof(cipher_text), key, sizeof(key)),
                 TURBO_CRYPTO_EBUFFER);
    check_equal(output_len, sizeof(decrypted));
    check_equal(turbo_crypto_xxtea_decrypt(
                     decrypted, sizeof(decrypted), &output_len,
                     cipher_text, sizeof(cipher_text) - 1U, key, sizeof(key)),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_xxtea_encrypt_size(0U, &output_len),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_xxtea_encrypt(
                     cipher_text, sizeof(cipher_text), &output_len,
                     plain_text, sizeof(plain_text), key,
                     TURBO_CRYPTO_XXTEA_KEY_SIZE + 1U),
                 TURBO_CRYPTO_EINVAL);
  }

  it("matches the RFC 8032 Ed448 empty-message vector") {
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE];
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE];

    check_equal(turbo_crypto_ed448_public_key(ed448_rfc8032_private_key,
                                               public_key),
                 TURBO_CRYPTO_OK);
    check_equal(public_key, ed448_rfc8032_public_key,
                         sizeof(public_key));
    check_equal(turbo_crypto_ed448_sign(ed448_rfc8032_private_key, NULL, 0,
                                         signature),
                 TURBO_CRYPTO_OK);
    check_equal(signature, ed448_rfc8032_signature,
                         sizeof(signature));
    check_equal(turbo_crypto_ed448_verify(public_key, NULL, 0, signature),
                 TURBO_CRYPTO_OK);

    signature[0] ^= 0x01U;
    check_equal(turbo_crypto_ed448_verify(public_key, NULL, 0, signature),
                 TURBO_CRYPTO_EVERIFY);
  }

  it("generates an Ed448 key pair that signs and verifies") {
    static const char message[] = "generated Ed448 key";
    uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE];
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE];
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE];

    check_equal(turbo_crypto_ed448_keygen(private_key, public_key),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_ed448_sign(private_key, message,
                                         sizeof(message) - 1U, signature),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_ed448_verify(public_key, message,
                                           sizeof(message) - 1U, signature),
                 TURBO_CRYPTO_OK);
    turbo_crypto_wipe(private_key, sizeof(private_key));
  }

  it("rejects invalid pointers and repeated finalization") {
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    uint8_t ed448_public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE];
    uint8_t ed448_signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE];
    uint8_t overlapping_keys[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE + 1U];
    turbo_crypto_sha256_ctx_t ctx;

    check_equal(turbo_crypto_sha256(NULL, 1, digest), TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_sha256("", 0, NULL), TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_hmac_sha256(NULL, 1, "", 0, digest),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_md5(NULL, 1, digest), TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_sha256_init(&ctx), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_final(&ctx, digest), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_sha256_update(&ctx, "x", 1),
                 TURBO_CRYPTO_ESTATE);
    check_equal(turbo_crypto_pbkdf2_hmac_sha256(
                     "password", 8, "salt", 4, 0, digest, sizeof(digest)),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_ed448_public_key(NULL, ed448_public_key),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_ed448_keygen(overlapping_keys,
                                           overlapping_keys + 1U),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_ed448_sign(ed448_rfc8032_private_key, NULL, 1,
                                         ed448_signature),
                 TURBO_CRYPTO_EINVAL);
    check_equal(turbo_crypto_ed448_verify(ed448_rfc8032_public_key, NULL, 1,
                                           ed448_rfc8032_signature),
                 TURBO_CRYPTO_EINVAL);
  }

  it("uses the platform CSPRNG and verifies bytes in constant time") {
    uint8_t first[TURBO_CRYPTO_SHA256_SIZE];
    uint8_t second[TURBO_CRYPTO_SHA256_SIZE];

    check_equal(turbo_crypto_random(first, sizeof(first)), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_random(second, sizeof(second)), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_verify(first, first, sizeof(first)),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_verify(first, second, sizeof(first)),
                 TURBO_CRYPTO_EVERIFY);
    memcpy(second, first, sizeof(first));
    check_equal(turbo_crypto_verify(first, second, sizeof(first)),
                 TURBO_CRYPTO_OK);
    turbo_crypto_wipe(second, sizeof(second));
    check_equal(turbo_crypto_random(NULL, 1), TURBO_CRYPTO_ERANDOM);
  }

  it("matches the BLAKE2b standard vector") {
    static const uint8_t expected[TURBO_CRYPTO_BLAKE2B_MAX_SIZE] = {
        0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d, 0x6a, 0x27, 0x97, 0xb6, 0x9f,
        0x12, 0xf6, 0xe9, 0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7, 0x4b, 0x12,
        0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1, 0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79,
        0x2d, 0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95, 0x18, 0xd3, 0x8a, 0xa8,
        0xdb, 0xf1, 0x92, 0x5a, 0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23};
    uint8_t actual[TURBO_CRYPTO_BLAKE2B_MAX_SIZE];

    check_equal(turbo_crypto_blake2b(actual, sizeof(actual), "abc", 3U), TURBO_CRYPTO_OK);
    check_equal(actual, expected, sizeof(expected));
    check_equal(turbo_crypto_blake2b(actual, 0U, "abc", 3U), TURBO_CRYPTO_EINVAL);
  }

  it("round-trips authenticated encryption and rejects a changed tag") {
    static const uint8_t plain_text[] = "authenticated payload";
    static const uint8_t associated_data[] = "header";
    uint8_t key[TURBO_CRYPTO_AEAD_KEY_SIZE] = {0};
    uint8_t nonce[TURBO_CRYPTO_AEAD_NONCE_SIZE] = {0};
    uint8_t mac[TURBO_CRYPTO_AEAD_MAC_SIZE];
    uint8_t cipher_text[sizeof(plain_text)];
    uint8_t unlocked[sizeof(plain_text)];

    check_equal(turbo_crypto_aead_lock(cipher_text, mac, key, nonce, associated_data,
                                        sizeof(associated_data) - 1U, plain_text,
                                        sizeof(plain_text)),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_aead_unlock(unlocked, mac, key, nonce, associated_data,
                                          sizeof(associated_data) - 1U, cipher_text,
                                          sizeof(cipher_text)),
                 TURBO_CRYPTO_OK);
    check_equal(unlocked, plain_text, sizeof(plain_text));

    mac[0] ^= 0x01U;
    check_equal(turbo_crypto_aead_unlock(unlocked, mac, key, nonce, associated_data,
                                          sizeof(associated_data) - 1U, cipher_text,
                                          sizeof(cipher_text)),
                 TURBO_CRYPTO_EVERIFY);
  }

  it("derives matching X25519 shared secrets") {
    uint8_t alice_secret[TURBO_CRYPTO_CURVE25519_SIZE] = {8U};
    uint8_t bob_secret[TURBO_CRYPTO_CURVE25519_SIZE] = {16U};
    uint8_t alice_public[TURBO_CRYPTO_CURVE25519_SIZE];
    uint8_t bob_public[TURBO_CRYPTO_CURVE25519_SIZE];
    uint8_t alice_shared[TURBO_CRYPTO_CURVE25519_SIZE];
    uint8_t bob_shared[TURBO_CRYPTO_CURVE25519_SIZE];

    check_equal(turbo_crypto_x25519_public_key(alice_public, alice_secret), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_x25519_public_key(bob_public, bob_secret), TURBO_CRYPTO_OK);
    check_not_equal(alice_public, bob_public, sizeof(alice_public));
    check_equal(turbo_crypto_x25519(alice_shared, alice_secret, bob_public), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_x25519(bob_shared, bob_secret, alice_public), TURBO_CRYPTO_OK);
    check_equal(alice_shared, bob_shared, sizeof(alice_shared));
  }

  it("signs and verifies with the Monocypher EdDSA adapter") {
    static const uint8_t message[] = "curve25519 EdDSA";
    uint8_t seed[TURBO_CRYPTO_CURVE25519_SIZE] = {3U};
    uint8_t secret_key[TURBO_CRYPTO_EDDSA_SECRET_KEY_SIZE];
    uint8_t public_key[TURBO_CRYPTO_CURVE25519_SIZE];
    uint8_t signature[TURBO_CRYPTO_EDDSA_SIGNATURE_SIZE];

    check_equal(turbo_crypto_eddsa_key_pair(secret_key, public_key, seed), TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_eddsa_sign(signature, secret_key, message, sizeof(message) - 1U),
                 TURBO_CRYPTO_OK);
    check_equal(turbo_crypto_eddsa_check(signature, public_key, message, sizeof(message) - 1U),
                 TURBO_CRYPTO_OK);
    signature[0] ^= 0x01U;
    check_equal(turbo_crypto_eddsa_check(signature, public_key, message, sizeof(message) - 1U),
                 TURBO_CRYPTO_EVERIFY);
    turbo_crypto_wipe(secret_key, sizeof(secret_key));
  }
}
