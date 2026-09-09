#ifndef CHTTP_JWT_POLICY_H
#define CHTTP_JWT_POLICY_H

/* Issuance and admission must enforce the same minimum HMAC key strength. */
enum { CHTTP_JWT_HS256_MIN_KEY_BYTES = 32u };

#endif
