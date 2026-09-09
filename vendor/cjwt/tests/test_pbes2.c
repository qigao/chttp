#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cjwt/cjwt.h"

suite("cjwt pbes2") {
  group("encrypt/decrypt") {
    it("round-trips with password") {
      cjwt_t jwt = {0};
      jwt.header.alg = alg_pbes2_hs256_a128kw;
      jwt.header.enc = enc_a128gcm;
      jwt.iss = "password-derived-issuer";
      jwt.sub = "secret-subject";

      const uint8_t password[] = "my-secret-password";
      char *token = NULL;

      cjwt_code_t rv = cjwt_encode(&jwt, password, sizeof(password)-1, &token);
      check_equal(CJWTE_OK, rv);
      check_not_null(token);

      cjwt_t *decrypted = NULL;
      rv = cjwt_decode(token, strlen(token), 0, password, sizeof(password)-1, 0, 0, &decrypted);
      check_equal(CJWTE_OK, rv);
      check_not_null(decrypted);
      check_equal("password-derived-issuer", decrypted->iss);
      check_equal(alg_pbes2_hs256_a128kw, decrypted->header.alg);

      free(token);
      json_free(jwt.header.private_headers);
      cjwt_destroy(decrypted);
    }
  }
}
