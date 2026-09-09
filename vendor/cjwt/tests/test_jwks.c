#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cjwt/cjwt.h"

suite("cjwt jwks") {
  group("parse") {
    it("parses a JWKS with RSA and EC keys") {
      const char *jwks_json = "{"
          "\"keys\": ["
              "{"
                  "\"kty\": \"RSA\","
                  "\"kid\": \"rsa-key-1\","
                  "\"use\": \"sig\","
                  "\"n\": \"0vx7agoebGcQSuuPiLJXZptN9nndrQmbXEps2aiAFbWhM78LhWx4cbbfAAtVT86zwu1RK7aPFFxuhDR1L6tSoc_BJECPebWKRXjBZCiFV4n3oknjhMstn64tZ_2W-5JsGY4Hc5n9yBXArwl93lqt7_RN5w6Cf0h4QyQ5v-65YGjQR0_FDW2QvzqY368QQMicAtaSqzs8KJZgnYb9c7d0zgdAZHzu6qMQvRL5hajrn1n91CbOpbISD08qNLyrdkt-bFTWhAI4vMQFh6WeZu0fM4lFd2NcRwr3XPksINHaQ-G_xBniIqbw0Ls1jF44-csFCur-kEgU8awapJzKnqDKgw\","
                  "\"e\": \"AQAB\""
              "},"
              "{"
                  "\"kty\": \"EC\","
                  "\"kid\": \"ec-key-1\","
                  "\"crv\": \"P-256\","
                  "\"x\": \"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
                  "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
              "}"
          "]"
      "}";

      cjwt_jwks_t *jwks = NULL;
      cjwt_code_t rv = cjwt_jwks_parse(jwks_json, &jwks);
      check_equal(CJWTE_OK, rv);
      check_not_null(jwks);
      check_equal(2, jwks->count);

      check_equal(CJWT_KTY_RSA, jwks->keys[0]->kty);
      check_equal("rsa-key-1", jwks->keys[0]->kid);

      check_equal(CJWT_KTY_EC, jwks->keys[1]->kty);
      check_equal("ec-key-1", jwks->keys[1]->kid);

      cjwt_jwks_destroy(jwks);
    }
  }
}
