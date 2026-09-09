#include "tinytest.h"
#include "cjwt.h"
#include <string.h>
#include <stdlib.h>

suite("cjwt jwe") {
  group("dir/a128gcm") {
    it("encodes and decodes") {
      cjwt_t jwt = {0};
      jwt.header.alg = alg_dir;
      jwt.header.enc = enc_a128gcm;
      jwt.iss = "jwe_issuer";

      const uint8_t key[16] = {0};
      char *output = NULL;
      cjwt_code_t rv = cjwt_encode(&jwt, key, sizeof(key), &output);

      check_equal(CJWTE_OK, rv);
      check_not_null(output);

      cjwt_t *decoded = NULL;
      rv = cjwt_decode(output, strlen(output), 0, key, sizeof(key), 0, 0, &decoded);

      check_equal(CJWTE_OK, rv);
      check_equal("jwe_issuer", decoded->iss);
      check_equal(alg_dir, decoded->header.alg);
      check_equal(enc_a128gcm, decoded->header.enc);

      cjwt_destroy(decoded);
      free(output);
    }
  }
}
