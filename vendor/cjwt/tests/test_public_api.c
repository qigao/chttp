#include <cjwt/cjwt.h>
#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

spec("Public JWT C API") {
    it("round trips a signed token through public headers and target") {
        static const uint8_t key[] = "installed-consumer-test-key";
        cjwt_t token = {0};
        cjwt_t *decoded = NULL;
        char *encoded = NULL;
        token.header.alg = alg_hs256;
        token.iss = "installed-consumer";
        check_equal(cjwt_encode(&token, key, sizeof(key) - 1u, &encoded), CJWTE_OK);
        check_equal(cjwt_decode(encoded, strlen(encoded), OPT_ALLOW_ONLY_HS_ALG,
                                key, sizeof(key) - 1u, 0, 0, &decoded), CJWTE_OK);
        check_equal(decoded->iss, "installed-consumer");
        cjwt_destroy(decoded);
        free(encoded);
    }
}
