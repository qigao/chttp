#include "tinytest.h"
#include "turbo_crypto.h"

#include <stdint.h>

enum { BENCHMARK_INPUT_SIZE = 1024 * 1024 };

static uint8_t benchmark_input[BENCHMARK_INPUT_SIZE];

spec("turbo crypto benchmark") {
  before_all() {
    for (size_t i = 0; i < sizeof(benchmark_input); ++i) {
      benchmark_input[i] = (uint8_t)(i * 31U + 17U);
    }
  }

  bench("one-shot digest throughput") {
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    int rc = TURBO_CRYPTO_OK;

    benchmark_bytes("SHA-256 1 MiB", 100, sizeof(benchmark_input)) {
      rc = turbo_crypto_sha256(benchmark_input, sizeof(benchmark_input), digest);
    }
    check_equal(rc, TURBO_CRYPTO_OK);

    benchmark_bytes("MD5 1 MiB", 100, sizeof(benchmark_input)) {
      rc = turbo_crypto_md5(benchmark_input, sizeof(benchmark_input), digest);
    }
    check_equal(rc, TURBO_CRYPTO_OK);
  }

  bench("SigV4-sized HMAC throughput") {
    static const uint8_t key[32] = {0x42};
    static const uint8_t message[128] = {0x24};
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    int rc = TURBO_CRYPTO_OK;

    benchmark_ops("HMAC-SHA256 128 bytes", 10000, 1) {
      rc = turbo_crypto_hmac_sha256(key, sizeof(key), message, sizeof(message),
                                    digest);
    }
    check_equal(rc, TURBO_CRYPTO_OK);
  }

  bench("password KDF throughput") {
    static const uint8_t salt[16] = {0x5a};
    uint8_t derived_key[TURBO_CRYPTO_SHA256_SIZE];
    int rc = TURBO_CRYPTO_OK;

    benchmark_ops("PBKDF2-HMAC-SHA256 100k", 5, 1) {
      rc = turbo_crypto_pbkdf2_hmac_sha256(
          "benchmark-password", 18, salt, sizeof(salt), 100000,
          derived_key, sizeof(derived_key));
    }
    check_equal(rc, TURBO_CRYPTO_OK);
  }
}
