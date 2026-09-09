# Turbo Crypto

`turbo_crypto` is a private static dependency of CHTTP's internal JWT support.
It combines the repository's vendored SHA-256 implementation, Monocypher's
verification and secure-wipe primitives, the Salts platform CSPRNG, libecc's
Ed448 implementation, and a compact MD5 implementation.

The internal API supplies the algorithms CJWT needs when BoringSSL does not
provide a compatible primitive, while keeping third-party headers and types
private:

- SHA-256 and incremental SHA-256 for AWS payload hashing.
- HMAC-SHA256 for AWS Signature Version 4.
- PBKDF2-HMAC-SHA256 for existing password and JOSE PBES2 formats.
- Constant-time verification, secure memory wiping, and random bytes.
- RFC 8032 pure Ed448 key generation, public-key derivation, signing, and
  verification.
- BLAKE2b, XChaCha20-Poly1305 AEAD, Argon2, X25519, Curve25519 EdDSA,
  ChaCha20, Poly1305, and Elligator through validated one-shot adapters.
- XXTEA for compatibility with existing xxtea-c ciphertexts. XXTEA is
  deterministic and unauthenticated; new formats must use the AEAD API.
- MD5 only for S3 fields that explicitly require it, including Content-MD5,
  multipart ETags, and SSE-C key checksums.

MD5 is implemented from the algorithm and test vectors in RFC 1321; no RFC
sample source code was copied. HMAC follows RFC 2104 and is verified against
RFC 4231 vectors. SHA-256 and Monocypher are private sources of the exported
`turbo_crypto` static library. Temporary HMAC key material is erased with
Monocypher `crypto_wipe()`.

Only CHTTP's private CJWT target includes `turbo_crypto.h` and links this
target. It is not installed or exported; no Monocypher header or type crosses
the CHTTP public API boundary.

libecc is vendored from https://github.com/libecc/libecc at commit
`6e8f214f41f65d5f30b04da75472f9c24f2100db`. Salts selects the upstream
BSD license and compiles only WEI448, SHAKE256, and EDDSA448 into the same
`turbo_crypto` archive. The public adapter accepts RFC 8032's 57-byte private
seed and keeps all libecc types and headers private. libecc's scalar-blinding
randomness is supplied by Salts' operating-system CSPRNG.

xxtea-c is vendored from https://github.com/xxtea/xxtea-c at commit
`7ec961540996934d939572d885ea1d5b21689688` under the MIT license. Its public
header remains private; callers use the capacity-checked `turbo_crypto_xxtea_*`
adapters and the shared `TURBO_CRYPTO_*` error model.

## Ed448 usage

```c
#include "turbo_crypto.h"

#include <string.h>

int main(void) {
    static const char message[] = "signed message";
    uint8_t private_key[TURBO_CRYPTO_ED448_PRIVATE_KEY_SIZE];
    uint8_t public_key[TURBO_CRYPTO_ED448_PUBLIC_KEY_SIZE];
    uint8_t signature[TURBO_CRYPTO_ED448_SIGNATURE_SIZE];
    int rc = turbo_crypto_ed448_keygen(private_key, public_key);

    if (rc == TURBO_CRYPTO_OK) {
        rc = turbo_crypto_ed448_sign(private_key, message,
                                     strlen(message), signature);
    }
    if (rc == TURBO_CRYPTO_OK) {
        rc = turbo_crypto_ed448_verify(public_key, message,
                                       strlen(message), signature);
    }
    turbo_crypto_wipe(private_key, sizeof(private_key));
    return rc == TURBO_CRYPTO_OK ? 0 : 1;
}
```

## Architecture decision

CHTTP retains BoringSSL for JWT algorithms it supports: RSA, ECDSA,
SHA-384/SHA-512, AES-GCM/AES-KW, and RSA-OAEP. CJWT uses `turbo_crypto` for
the unavailable compatible primitives: HS256, PBES2-HS256, random bytes,
constant-time verification, secure wiping, and Ed448. This keeps CHTTP's
public ABI unchanged and avoids a fallback that changes JOSE wire semantics.

The validation boundary is the turbo-crypto vector suite and the CJWT JWT,
JWK, JWKS, JWE, and PBES2 tests.

## SIMD boundary

SHA-256 and MD5 compression blocks are serially chained for a single stream.
Portable SIMD is therefore useful mainly for multiple independent messages,
not the single-file S3 upload path. A SIMDe SSE2 experiment on HMAC's two
independent 64-byte pads did not improve the end-to-end SigV4-sized HMAC
benchmark, so it is not retained. The benchmark target provides a baseline
for a future measured multi-buffer implementation without changing scalar
semantics on Windows, Linux, and Android.

References:

- https://www.rfc-editor.org/rfc/rfc1321
- https://www.rfc-editor.org/rfc/rfc2104
- https://www.rfc-editor.org/rfc/rfc4231
- https://www.rfc-editor.org/rfc/rfc8032
- https://csrc.nist.gov/pubs/fips/180-4/upd1/final
