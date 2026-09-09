#include "tinytest.h"
#include "cjwt.h"
#include "turbo_crypto.h"
#include <string.h>
#include <stdlib.h>

static const uint8_t ed448_private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE] = {
    0x6c, 0x82, 0xa5, 0x62, 0xcb, 0x80, 0x8d, 0x10,
    0xd6, 0x32, 0xbe, 0x89, 0xc8, 0x51, 0x3e, 0xbf,
    0x6c, 0x92, 0x9f, 0x34, 0xdd, 0xfa, 0x8c, 0x9f,
    0x63, 0xc9, 0x96, 0x0e, 0xf6, 0xe3, 0x48, 0xa3,
    0x52, 0x8c, 0x8a, 0x3f, 0xcc, 0x2f, 0x04, 0x4e,
    0x39, 0xa3, 0xfc, 0x5b, 0x94, 0x49, 0x2f, 0x8f,
    0x03, 0x2e, 0x75, 0x49, 0xa2, 0x00, 0x98, 0xf9,
    0x5b
};

static const uint8_t ed448_public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE] = {
    0x5f, 0xd7, 0x44, 0x9b, 0x59, 0xb4, 0x61, 0xfd,
    0x2c, 0xe7, 0x87, 0xec, 0x61, 0x6a, 0xd4, 0x6a,
    0x1d, 0xa1, 0x34, 0x24, 0x85, 0xa7, 0x0e, 0x1f,
    0x8a, 0x0e, 0xa7, 0x5d, 0x80, 0xe9, 0x67, 0x78,
    0xed, 0xf1, 0x24, 0x76, 0x9b, 0x46, 0xc7, 0x06,
    0x1b, 0xd6, 0x78, 0x3d, 0xf1, 0xe5, 0x0f, 0x6c,
    0xd1, 0xfa, 0x1a, 0xbe, 0xaf, 0xe8, 0x25, 0x61,
    0x80
};

static const char ed448_public_jwk[] =
    "{\"kty\":\"OKP\",\"crv\":\"Ed448\","
    "\"x\":\"X9dEm1m0Yf0s54fsYWrUah2hNCSFpw4fig6nXYDpZ3jt8SR2m0bHBhvWeD3x5Q9s0foavq_oJWGA\"}";

static const char ed448_private_jwk[] =
    "{\"kty\":\"OKP\",\"crv\":\"Ed448\","
    "\"d\":\"bIKlYsuAjRDWMr6JyFE-v2ySnzTd-oyfY8mWDvbjSKNSjIo_zC8ETjmj_FuUSS-PAy51SaIAmPlb\"}";

static cjwt_code_t encode_ed448_token(char **token)
{
  cjwt_t jwt = {0};
  jwt.header.alg = alg_eddsa;
  jwt.iss = "ed448_issuer";
  return cjwt_encode(&jwt, ed448_private_key, sizeof(ed448_private_key), token);
}

suite("cjwt jwk") {
  group("parse") {
    it("parses RSA JWK") {
      const char *jwk_json = "{"
          "\"kty\":\"RSA\","
          "\"kid\":\"test_kid\","
          "\"use\":\"sig\","
          "\"n\":\"o76m_D87p9B...example\","
          "\"e\":\"AQAB\""
      "}";

      cjwt_jwk_t *jwk = NULL;
      cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);

      check_equal(CJWTE_OK, rv);
      check_not_null(jwk);
      check_equal(CJWT_KTY_RSA, jwk->kty);
      check_equal("test_kid", jwk->kid);
      check_equal("sig", jwk->use);

      cjwt_jwk_destroy(jwk);
    }

    it("parses EC JWK") {
      const char *jwk_json = "{"
          "\"kty\":\"EC\","
          "\"crv\":\"P-256\","
          "\"x\":\"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
          "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
      "}";

      cjwt_jwk_t *jwk = NULL;
      cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);

      check_equal(CJWTE_OK, rv);
      check_not_null(jwk);
      check_equal(CJWT_KTY_EC, jwk->kty);

      cjwt_jwk_destroy(jwk);
    }
  }

  group("verify") {
    it("imports an RSA public key before rejecting a bad signature") {
      const char *jwk_json = "{"
          "\"kty\":\"RSA\","
          "\"n\":\"0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-65YGjQR0_FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw\","
          "\"e\":\"AQAB\""
      "}";
      const char *token = "eyJhbGciOiJSUzI1NiJ9.e30.AA";
      cjwt_jwk_t *jwk = NULL;
      cjwt_t *decoded = NULL;

      check_equal(cjwt_jwk_parse(jwk_json, &jwk), CJWTE_OK);
      check_equal(cjwt_decode_with_jwk(token, strlen(token), 0, jwk,
                                        0, 0, &decoded),
                   CJWTE_SIGNATURE_VALIDATION_FAILED);
      check_null(decoded);
      cjwt_jwk_destroy(jwk);
    }

    it("imports an EC public key before rejecting a bad signature") {
      const char *jwk_json = "{"
          "\"kty\":\"EC\","
          "\"crv\":\"P-256\","
          "\"x\":\"8_BkzVLg33jRwRoPBDkv4QpjrjqfZgdGyMSo5gh1S0M\","
          "\"y\":\"E93dnEz-04t1TuA8GUG6m93QkOC_D_VBRsrWvArqofE\""
      "}";
      const char *token = "eyJhbGciOiJFUzI1NiJ9.e30.AA";
      cjwt_jwk_t *jwk = NULL;
      cjwt_t *decoded = NULL;

      check_equal(cjwt_jwk_parse(jwk_json, &jwk), CJWTE_OK);
      check_equal(cjwt_decode_with_jwk(token, strlen(token), 0, jwk,
                                        0, 0, &decoded),
                   CJWTE_SIGNATURE_VALIDATION_FAILED);
      check_null(decoded);
      cjwt_jwk_destroy(jwk);
    }

    it("round-trips Ed448 with raw RFC 8032 keys") {
      char *token = NULL;
      cjwt_t *decoded = NULL;

      check_equal(encode_ed448_token(&token), CJWTE_OK);
      check_not_null(token);
      check_equal(cjwt_decode(token, strlen(token), 0, ed448_public_key,
                               sizeof(ed448_public_key), 0, 0, &decoded),
                   CJWTE_OK);
      check_not_null(decoded);
      check_equal(decoded->iss, "ed448_issuer");

      cjwt_destroy(decoded);
      free(token);
    }

    it("verifies Ed448 with a public JWK") {
      char *token = NULL;
      cjwt_jwk_t *jwk = NULL;
      cjwt_t *decoded = NULL;

      check_equal(encode_ed448_token(&token), CJWTE_OK);
      check_equal(cjwt_jwk_parse(ed448_public_jwk, &jwk), CJWTE_OK);
      check_equal(cjwt_decode_with_jwk(token, strlen(token), 0, jwk,
                                        0, 0, &decoded),
                   CJWTE_OK);
      check_not_null(decoded);
      check_equal(decoded->iss, "ed448_issuer");

      cjwt_destroy(decoded);
      cjwt_jwk_destroy(jwk);
      free(token);
    }

    it("derives the Ed448 public key from a private JWK") {
      char *token = NULL;
      cjwt_jwk_t *jwk = NULL;
      cjwt_t *decoded = NULL;

      check_equal(encode_ed448_token(&token), CJWTE_OK);
      check_equal(cjwt_jwk_parse(ed448_private_jwk, &jwk), CJWTE_OK);
      check_equal(cjwt_decode_with_jwk(token, strlen(token), 0, jwk,
                                        0, 0, &decoded),
                   CJWTE_OK);
      check_not_null(decoded);

      cjwt_destroy(decoded);
      cjwt_jwk_destroy(jwk);
      free(token);
    }

    it("rejects an Ed448 token with a modified signature") {
      char *token = NULL;
      cjwt_t *decoded = NULL;
      char *signature = NULL;

      check_equal(encode_ed448_token(&token), CJWTE_OK);
      signature = strrchr(token, '.');
      check_not_null(signature);
      check_true(signature[1] != '\0');
      signature[1] = signature[1] == 'A' ? 'B' : 'A';

      check_equal(cjwt_decode(token, strlen(token), 0, ed448_public_key,
                               sizeof(ed448_public_key), 0, 0, &decoded),
                   CJWTE_SIGNATURE_VALIDATION_FAILED);
      check_null(decoded);
      free(token);
    }

    it("rejects an Ed448 token verified with the wrong key") {
      char *token = NULL;
      cjwt_t *decoded = NULL;
      uint8_t wrong_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE];

      memcpy(wrong_key, ed448_public_key, sizeof(wrong_key));
      wrong_key[0] ^= 0x01U;
      check_equal(encode_ed448_token(&token), CJWTE_OK);
      check_equal(cjwt_decode(token, strlen(token), 0, wrong_key,
                               sizeof(wrong_key), 0, 0, &decoded),
                   CJWTE_SIGNATURE_VALIDATION_FAILED);
      check_null(decoded);

      free(token);
    }

    it("rejects inconsistent Ed448 public and private JWK fields") {
      const char *jwk_json =
          "{\"kty\":\"OKP\",\"crv\":\"Ed448\","
          "\"x\":\"X9dEm1m0Yf0s54fsYWrUah2hNCSFpw4fig6nXYDpZ3jt8SR2m0bHBhvWeD3x5Q9s0foavq_oJWGA\","
          "\"d\":\"bYKlYsuAjRDWMr6JyFE-v2ySnzTd-oyfY8mWDvbjSKNSjIo_zC8ETjmj_FuUSS-PAy51SaIAmPlb\"}";
      char *token = NULL;
      cjwt_jwk_t *jwk = NULL;
      cjwt_t *decoded = NULL;

      check_equal(encode_ed448_token(&token), CJWTE_OK);
      check_equal(cjwt_jwk_parse(jwk_json, &jwk), CJWTE_OK);
      check_equal(cjwt_decode_with_jwk(token, strlen(token), 0, jwk,
                                        0, 0, &decoded),
                   CJWTE_INVALID_PARAMETERS);
      check_null(decoded);

      cjwt_jwk_destroy(jwk);
      free(token);
    }
  }
}
