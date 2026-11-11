# cjwt crypto primitives

JWT/JWE uses the private `chttp_cjwt_crypto` build target. The implementation
comes from SaltsUtils commit `234c4a73bcefc455d48b3550ec2142d7ff1c3f5a`, directory
`crypto/`, under Apache-2.0. The source is recorded in the SaltsUtils Git history.
The bundled libecc snapshot retains its upstream BSD/GPLv2 license and provenance.
CHttp selects the BSD license.

The existing vcpkg-cache GmSSL package supplies RSA, HMAC, PBKDF2, randomness,
AES-GCM and AES key wrap. libecc supplies the four JOSE EC curves and Ed25519/
Ed448. These operations are required by the existing cjwt algorithms and
interoperability vectors. The published CNet Crypto interface supplies its TLS
operations and does not export this complete JOSE primitive interface.

The adapter symbols use `cjwt_crypto_` and belong to this private target.
libecc SHA symbols use `chttp_cjwt_libecc_` to coexist with GmSSL contexts.
Provider sources use hidden ELF visibility. The standalone SHA-256 wrapper from
the original source is omitted because cjwt uses GmSSL's digest interface.
The target and its headers stay in the cjwt build tree. Installation includes
the libecc license.

## Ownership and failure contracts

Key import copies caller buffers into immutable bounded key storage. The
synchronous JWS/JWE operation owns the imported key and destroys it on each
exit path. Destruction clears secret material before release. Message and
output buffers belong to the caller and are borrowed for the operation.
The private header documents capacities, permitted overlap and output state
for invalid input, allocation failure and authentication failure.

Randomness is required for private-key blinding and encryption. Random-source
failure is returned to the JOSE caller. Authenticated decryption releases
plaintext only after tag verification. Independent operations have separate
contexts and immutable imported keys. No background work retains caller data.

## Validation and risks

`chttp_cjwt_*` CTests cover the existing algorithms, PEM/JWK imports, malformed
inputs, tampering, long PBES2 salts, JWS vectors and JWE interoperability.
HTTP JWT, WebSocket, CSRF and S3 tests verify their product paths.

HIGH security risk remains in key parsing and signature/decryption boundaries.
The implementation preserves the prior provider and must be qualified by the
formal tests on each supported platform. Functional tests do not establish a
cryptographic security audit. Changing providers requires reviewing all
algorithm, key-format, ownership and error contracts.

Rollback restores the preceding cjwt sources together with their matching
SDK dependency composition.
