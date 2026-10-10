#include <tinytest.h>
#include "cjwt.h"
#include <stdlib.h>
#include <string.h>
#include "salts_vectors.h"

static size_t decode_hex(const char *text, uint8_t *output) {
    size_t i, size = strlen(text)/2;
    for (i = 0; i < size; ++i) {
        unsigned char a = (unsigned char)text[2*i], b = (unsigned char)text[2*i+1];
        output[i] = (uint8_t)(((a <= '9' ? a-'0' : a-'a'+10) << 4) |
                              (b <= '9' ? b-'0' : b-'a'+10));
    }
    return size;
}

spec("cjwt Salts backend interoperability") {
    it("verifies independent JWS vectors using PEM and public/private JWK imports") {
        size_t i, k;
        for (i = 0; i < sizeof(jws_vectors)/sizeof(jws_vectors[0]); ++i) {
            uint32_t options = i < 3 ? OPT_ALLOW_ONLY_HS_ALG : 0;
            cjwt_t *decoded = NULL;
            check_equal(cjwt_decode(jws_vectors[i].token, strlen(jws_vectors[i].token), options,
                (const uint8_t *)jws_vectors[i].public_key, strlen(jws_vectors[i].public_key),
                0, 0, &decoded), CJWTE_OK);
            check_not_null(decoded);
            if (decoded) check_equal(decoded->iss, "migration");
            cjwt_destroy(decoded);
            for (k = 0; k < 2; ++k) {
                cjwt_jwk_t *jwk = NULL;
                decoded = NULL;
                check_equal(cjwt_jwk_parse(k ? jws_vectors[i].private_jwk : jws_vectors[i].public_jwk, &jwk), CJWTE_OK);
                /* The existing JWS JWK API admits asymmetric keys only. */
                if (i < 3) {
                    check_equal(cjwt_decode_with_jwk(jws_vectors[i].token, strlen(jws_vectors[i].token),
                        options, jwk, 0, 0, &decoded), CJWTE_SIGNATURE_UNSUPPORTED_ALG);
                    check_null(decoded); cjwt_jwk_destroy(jwk); continue;
                }
                check_equal(cjwt_decode_with_jwk(jws_vectors[i].token, strlen(jws_vectors[i].token),
                    options, jwk, 0, 0, &decoded), CJWTE_OK);
                check_not_null(decoded);
                if (decoded) check_equal(decoded->iss, "migration");
                cjwt_destroy(decoded); cjwt_jwk_destroy(jwk);
            }
        }
    }

    it("signs with every supported JWS algorithm and rejects a changed signature") {
        size_t i;
        for (i = 0; i < sizeof(jws_vectors)/sizeof(jws_vectors[0]); ++i) {
            uint32_t options = i < 3 ? OPT_ALLOW_ONLY_HS_ALG : 0;
            cjwt_t jwt = {0}, *decoded = NULL;
            char *token = NULL, *signature;
            jwt.header.alg = jws_vectors[i].alg; jwt.iss = "migration";
            check_equal(cjwt_encode(&jwt, (const uint8_t *)jws_vectors[i].private_key,
                strlen(jws_vectors[i].private_key), &token), CJWTE_OK);
            check_not_null(token);
            if (!token) continue;
            check_equal(cjwt_decode(token, strlen(token), options, (const uint8_t *)jws_vectors[i].public_key,
                strlen(jws_vectors[i].public_key), 0, 0, &decoded), CJWTE_OK);
            cjwt_destroy(decoded); decoded = NULL;
            signature = strrchr(token, '.') + 1;
            *signature = *signature == 'A' ? 'B' : 'A';
            check_not_equal(cjwt_decode(token, strlen(token), options, (const uint8_t *)jws_vectors[i].public_key,
                strlen(jws_vectors[i].public_key), 0, 0, &decoded), CJWTE_OK);
            check_null(decoded);
            cjwt_destroy(decoded); free(token);
        }
    }

    it("decrypts independent JWE vectors for all key-management and AES-GCM sizes") {
        size_t i;
        for (i = 0; i < sizeof(jwe_vectors)/sizeof(jwe_vectors[0]); ++i) {
            uint8_t bytes[32];
            const uint8_t *key = bytes;
            size_t key_size = decode_hex(jwe_vectors[i].key_hex, bytes);
            cjwt_t *decoded = NULL;
            int rsa = jwe_vectors[i].alg == alg_rsa_oaep || jwe_vectors[i].alg == alg_rsa_oaep_256;
            if (rsa) { key = (const uint8_t *)rsa_private_pem; key_size = strlen(rsa_private_pem); }
            check_equal(cjwt_decode(jwe_vectors[i].token, strlen(jwe_vectors[i].token), 0,
                key, key_size, 0, 0, &decoded), CJWTE_OK);
            check_not_null(decoded);
            if (decoded) check_equal(decoded->iss, "migration");
            cjwt_destroy(decoded);
            if (rsa) {
                cjwt_jwk_t *jwk = NULL;
                decoded = NULL;
                check_equal(cjwt_jwk_parse(rsa_private_jwk, &jwk), CJWTE_OK);
                check_equal(cjwt_decode_with_jwk(jwe_vectors[i].token, strlen(jwe_vectors[i].token),
                    0, jwk, 0, 0, &decoded), CJWTE_OK);
                check_not_null(decoded);
                cjwt_destroy(decoded); cjwt_jwk_destroy(jwk);
            }
        }
    }

    it("encrypts every JWE combination and rejects authentication failure without claims") {
        size_t i;
        for (i = 0; i < sizeof(jwe_vectors)/sizeof(jwe_vectors[0]); ++i) {
            uint8_t bytes[32];
            const uint8_t *encrypt_key = bytes, *decrypt_key = bytes;
            size_t encrypt_size = decode_hex(jwe_vectors[i].key_hex, bytes), decrypt_size = encrypt_size;
            cjwt_t jwt = {0}, *decoded = NULL;
            char *token = NULL, *tag;
            jwt.header.alg = jwe_vectors[i].alg; jwt.header.enc = jwe_vectors[i].enc; jwt.iss = "migration";
            if (jwt.header.alg == alg_rsa_oaep || jwt.header.alg == alg_rsa_oaep_256) {
                encrypt_key = (const uint8_t *)rsa_public_pem; encrypt_size = strlen(rsa_public_pem);
                decrypt_key = (const uint8_t *)rsa_private_pem; decrypt_size = strlen(rsa_private_pem);
            }
            if (*jwe_vectors[i].p2s) {
                jwt.header.private_headers = json_create_object();
                json_object_set_string(jwt.header.private_headers, "p2s", jwe_vectors[i].p2s);
                json_object_set_number(jwt.header.private_headers, "p2c", 2);
            }
            check_equal(cjwt_encode(&jwt, encrypt_key, encrypt_size, &token), CJWTE_OK);
            check_not_null(token);
            if (token) {
                check_equal(cjwt_decode(token, strlen(token), 0, decrypt_key, decrypt_size, 0, 0, &decoded), CJWTE_OK);
                check_not_null(decoded);
                if (decoded) check_equal(decoded->iss, "migration");
                cjwt_destroy(decoded); decoded = NULL;
                tag = strrchr(token, '.') + 1; *tag = *tag == 'A' ? 'B' : 'A';
                check_not_equal(cjwt_decode(token, strlen(token), 0, decrypt_key, decrypt_size, 0, 0, &decoded), CJWTE_OK);
                check_null(decoded); cjwt_destroy(decoded); free(token);
            }
            json_free(jwt.header.private_headers);
        }
    }
}
