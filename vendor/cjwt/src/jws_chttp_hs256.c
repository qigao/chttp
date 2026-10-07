/* CHttp-private cjwt JWS backend.
 *
 * CHttp's public JWT contract is HS256 only. Keep this vendor adapter narrow:
 * no OpenSSL/GmSSL provider API, no asymmetric/JWK execution, no fallback.
 */
#include "jws.h"

#include <cmeta_crypto.h>

#include <stdlib.h>
#include <string.h>

cjwt_code_t jws_verify_signature(const cjwt_t *jwt, const struct sig_input *in)
{
    uint8_t mac[SALTS_SHA256_DIGEST_BYTES];
    int equal = 0;
    int status;

    if (!jwt || !in) return CJWTE_INVALID_PARAMETERS;
    if (jwt->header.alg != alg_hs256) return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
    if (!in->key.data || in->key.len == 0) return CJWTE_SIGNATURE_MISSING_KEY;
    if (in->sig.len != sizeof(mac)) return CJWTE_SIGNATURE_VALIDATION_FAILED;

    status = cmeta_hmac_sha256(in->key.data, in->key.len,
                               in->full.data, in->full.len, mac);
    if (status == SALTS_OK)
        status = cmeta_crypto_equal(in->sig.data, mac, sizeof(mac), &equal);
    cmeta_crypto_clear(mac, sizeof(mac));
    return status == SALTS_OK && equal ? CJWTE_OK
                                       : CJWTE_SIGNATURE_VALIDATION_FAILED;
}

cjwt_code_t jws_sign(const cjwt_alg_t alg, const uint8_t *full, size_t full_len,
                     const uint8_t *key, size_t key_len,
                     uint8_t **sig, size_t *sig_len)
{
    uint8_t *output;
    int status;

    if (!sig || !sig_len || (!full && full_len != 0))
        return CJWTE_INVALID_PARAMETERS;
    *sig = NULL;
    *sig_len = 0;
    if (alg != alg_hs256) return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
    if (!key || key_len == 0) return CJWTE_SIGNATURE_MISSING_KEY;

    output = (uint8_t *)malloc(SALTS_SHA256_DIGEST_BYTES);
    if (!output) return CJWTE_OUT_OF_MEMORY;
    status = cmeta_hmac_sha256(key, key_len, full, full_len, output);
    if (status != SALTS_OK) {
        cmeta_crypto_clear(output, SALTS_SHA256_DIGEST_BYTES);
        free(output);
        return CJWTE_SIGNATURE_VALIDATION_FAILED;
    }

    *sig = output;
    *sig_len = SALTS_SHA256_DIGEST_BYTES;
    return CJWTE_OK;
}

cjwt_code_t jws_jwk_to_pkey(const cjwt_jwk_t *jwk, void **pkey,
                            jws_pkey_type_t *pkey_type)
{
    (void)jwk;
    if (pkey) *pkey = NULL;
    if (pkey_type) *pkey_type = JWS_PKEY_UNSUPPORTED;
    return CJWTE_SIGNATURE_UNSUPPORTED_ALG;
}

void jws_pkey_free(void *pkey, jws_pkey_type_t pkey_type)
{
    (void)pkey;
    (void)pkey_type;
}
