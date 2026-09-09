/**
 * cjwt Full Use Case Example
 * 
 * This example demonstrates:
 * 1. Creating and signing a JWS (JSON Web Signature).
 * 2. Creating and encrypting a JWE (JSON Web Encryption).
 * 3. Using JWK (JSON Web Key) for crypto operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cjwt/cjwt.h"

void demo_jws_signing() {
    printf("--- Use Case 1: Signed JWT (JWS) ---\n");
    
    cjwt_t jwt = {0};
    jwt.header.alg = alg_hs256;
    jwt.iss = "auth-server";
    jwt.sub = "user-1234";
    
    // Some private claims
    jwt.private_claims = json_create_object();
    json_object_set_string(jwt.private_claims, "role", "admin");

    const char *key = "very-secret-shared-key";
    char *token = NULL;

    // Encode and sign
    cjwt_code_t rv = cjwt_encode(&jwt, (const uint8_t *)key, strlen(key), &token);
    if (rv == CJWTE_OK) {
        printf("Generated JWS: %s\n", token);
        
        // Decode and verify
        cjwt_t *decoded = NULL;
        rv = cjwt_decode(token, strlen(token), OPT_ALLOW_ONLY_HS_ALG, (const uint8_t *)key, strlen(key), 0, 0, &decoded);
        
        if (rv == CJWTE_OK) {
            printf("Decoded Issuer: %s\n", decoded->iss);
            printf("Decoded Role: %s\n", json_get_string(decoded->private_claims, "role"));
            cjwt_destroy(decoded);
        } else {
            printf("Decoding failed!\n");
        }
        free(token);
    }
    json_free(jwt.private_claims);
    printf("\n");
}

void demo_jwe_encryption() {
    printf("--- Use Case 2: Encrypted JWT (JWE) ---\n");
    
    cjwt_t jwt = {0};
    jwt.header.alg = alg_dir;      // Direct Encryption
    jwt.header.enc = enc_a128gcm;  // Using AES-128-GCM
    jwt.iss = "secure-vault";
    jwt.sub = "extremely-secret-data";

    // 128-bit key for AES-128-GCM
    const uint8_t secret_key[16] = "1234567890123456";
    char *token = NULL;

    // Encrypt and encode
    cjwt_code_t rv = cjwt_encode(&jwt, secret_key, sizeof(secret_key), &token);
    if (rv == CJWTE_OK) {
        printf("Generated JWE: %s\n", token);
        
        // Decrypt and decode
        cjwt_t *decrypted = NULL;
        rv = cjwt_decode(token, strlen(token), 0, secret_key, sizeof(secret_key), 0, 0, &decrypted);
        
        if (rv == CJWTE_OK) {
            printf("Decrypted Issuer: %s\n", decrypted->iss);
            cjwt_destroy(decrypted);
        } else {
            printf("Decryption failed!\n");
        }
        free(token);
    }
    printf("\n");
}

void demo_jwk_interop() {
    printf("--- Use Case 3: Using JSON Web Keys (JWK) ---\n");

    // A Sample Public EC Key in JWK format
    const char *jwk_json = "{"
        "\"kty\":\"EC\","
        "\"crv\":\"P-256\","
        "\"x\":\"f83OJ3D2x1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEUo\","
        "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\""
    "}";

    cjwt_jwk_t *jwk = NULL;
    cjwt_code_t rv = cjwt_jwk_parse(jwk_json, &jwk);
    
    if (rv == CJWTE_OK) {
        printf("Parsed JWK Key Type: %d (EC)\n", jwk->kty);
        
        /* 
           Imagine we received a token signed by this key.
           cjwt_decode_with_jwk() allows verification directly without 
           converting the JWK to PEM manually.
        */
        
        // cjwt_t *decoded = NULL;
        // rv = cjwt_decode_with_jwk(some_token, token_len, 0, jwk, 0, 0, &decoded);
        
        printf("JWK is ready for use in token verification.\n");
        cjwt_jwk_destroy(jwk);
    } else {
        printf("JWK parsing failed!\n");
    }
    printf("\n");
}

int main() {
    demo_jws_signing();
    demo_jwe_encryption();
    demo_jwk_interop();
    return 0;
}
