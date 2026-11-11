#ifndef SALTS_UTILS_CRYPTO_INTERNAL_H
#define SALTS_UTILS_CRYPTO_INTERNAL_H

#include <stddef.h>

static inline void cjwt_crypto_secure_zero(void *buffer, size_t size) {
  volatile unsigned char *p = (volatile unsigned char *)buffer;
  while (size-- != 0u) {
    *p++ = 0u;
  }
}

#endif /* SALTS_UTILS_CRYPTO_INTERNAL_H */
