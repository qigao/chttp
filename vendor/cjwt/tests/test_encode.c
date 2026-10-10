#include "tinytest.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

static void check_hmac_round_trip(cjwt_alg_t alg, const char *issuer,
                                  const char *key)
{
  cjwt_t jwt = {0};
  cjwt_t *decoded = NULL;
  char *output = NULL;
  cjwt_code_t rv;

  jwt.header.alg = alg;
  jwt.iss = (char *)issuer;

  rv = cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &output);
  check_equal(rv, CJWTE_OK);
  check_not_null(output);

  rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ONLY_HS_ALG,
                   (const uint8_t *)key, strlen(key), 0, 0, &decoded);
  check_equal(rv, CJWTE_OK);
  check_not_null(decoded);
  check_equal(decoded->iss, issuer);

  cjwt_destroy(decoded);
  free(output);
}

suite("cjwt encode") {
  group("alg none") {
    it("encodes and decodes without key") {
      cjwt_t jwt = {0};
      jwt.header.alg = alg_none;
      jwt.iss = "test_issuer";
      jwt.sub = "test_subject";

      char *output = NULL;
      cjwt_code_t rv = cjwt_encode(&jwt, NULL, 0, &output);

      check_equal(CJWTE_OK, rv);
      check_not_null(output);

      cjwt_t *decoded = NULL;
      rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ALG_NONE, NULL, 0, 0, 0, &decoded);
      check_equal(CJWTE_OK, rv);
      check_equal("test_issuer", decoded->iss);
      check_equal("test_subject", decoded->sub);

      cjwt_destroy(decoded);
      free(output);
    }
  }

  group("hs256") {
    it("encodes and decodes with key") {
      int64_t iat = 123456789;
      cjwt_t jwt = {0};
      jwt.header.alg = alg_hs256;
      jwt.iss = "hs_issuer";
      jwt.iat = &iat;

      const char *key = "secret_key";
      char *output = NULL;
      cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &output);

      check_equal(CJWTE_OK, rv);
      check_not_null(output);

      cjwt_t *decoded = NULL;
      rv = cjwt_decode(output, strlen(output), OPT_ALLOW_ONLY_HS_ALG,
                       (const uint8_t *)key, strlen(key), 0, 0, &decoded);
      check_equal(CJWTE_OK, rv);
      check_equal("hs_issuer", decoded->iss);
      check_equal(123456789LL, *decoded->iat);

      cjwt_destroy(decoded);
      free(output);
    }

    it("decodes issuer audience and registered times together") {
      static const char *const issuer = "issuer.example";
      static const char *const subject = "alice";
      static const char *const audience_value = "api.example";
      static const char *const key = "server-test-key";
      char *audience[] = {(char *)audience_value};
      int64_t issued_at = INT64_C(1700000000);
      int64_t not_before = INT64_C(1700000000);
      int64_t expires_at = INT64_C(3000000000);
      cjwt_t jwt = {.header = {.alg = alg_hs256},
                    .iss = (char *)issuer,
                    .sub = (char *)subject,
                    .aud = {.count = 1, .names = audience},
                    .iat = &issued_at,
                    .nbf = &not_before,
                    .exp = &expires_at};
      cjwt_t *decoded = NULL;
      char *output = NULL;

      check_equal(cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &output), CJWTE_OK);
      check_not_null(output);
      check_equal(cjwt_decode(output, strlen(output), OPT_ALLOW_ONLY_HS_ALG,
                              (const uint8_t *)key, strlen(key), INT64_C(1750000000), 0,
                              &decoded),
                  CJWTE_OK);
      check_not_null(decoded);
      check_equal(decoded->sub, subject);
      check_equal(decoded->aud.count, 1);
      check_equal(decoded->aud.names[0], audience_value);
      cjwt_destroy(decoded);
      free(output);
    }
  }

  group("hmac") {
    it("round-trips HS384") {
      check_hmac_round_trip(alg_hs384, "hs384_issuer", "hs384_secret_key");
    }

    it("round-trips HS512") {
      check_hmac_round_trip(alg_hs512, "hs512_issuer", "hs512_secret_key");
    }
  }
}
