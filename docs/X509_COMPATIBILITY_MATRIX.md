# CHttp X.509 compatibility matrix

This matrix describes evidence through CHttp's real HTTPS client and server
APIs. Primitive support or a CNet-only handshake is not promoted to a CHttp
claim. CHttp remains provider-neutral and links no OpenSSL, BoringSSL, or
GmSSL API directly; TLS and certificate validation belong to CNet.

| Certificate profile or condition | CHttp status | Executable evidence and limit |
| --- | --- | --- |
| RSA-2048 leaf, RSA issuer, SHA-256 signature | **verified** | `chttp_tls_test` performs HTTPS requests through the real client/server path and verifies middleware, sessions, connection reuse, peer certificate identity and required mTLS. |
| P-256 client leaf and issuer, ECDSA/SHA-256 | **verified for mTLS client authentication** | The same test requires the P-256 client identity and rejects TLS failure before HTTP dispatch. A P-256 server-only profile is not separately qualified. |
| P-384 issuer, P-256 server leaf, ECDSA/SHA-384 | **verified on the exact-source Linux gate** | The opt-in `CHTTP_P384_SHA384_ACCEPTANCE_TESTS` case completes a real HTTPS request and rejects wrong CA, wrong DNS name, expired leaf and tampered certificate signature before middleware dispatch. |
| Hostname and trust-anchor mismatch | **verified** | Both failures are exercised against a running HTTPS server; the middleware count remains unchanged. |
| Expired leaf and invalid certificate signature | **verified for the P-384/P-256 profile** | Fixed independently parseable fixtures reach the TLS handshake but never HTTP middleware. |
| TLS 1.2/1.3, negotiated cipher and signature-scheme reporting | **not yet verified in CHttp** | CNet owns negotiation; the CHttp test does not currently expose or record the negotiated tuple. |
| RSA-3072, RSA-PSS certificate signatures, SHA-512, Ed25519 | **not yet verified** | No CHttp product fixture or exact-source gate exists. |
| Root/intermediate/leaf with omitted intermediate | **not yet verified** | Current fixtures use a root-to-leaf chain. |
| Not-yet-valid leaf, invalid CA constraints, key usage or EKU | **not yet verified** | These negative cases require fixed product fixtures and pre-dispatch assertions. |
| SM2/SM3 and TLCP | **unsupported by the CHttp HTTPS profile** | ShangMi TLS/TLCP qualification is a separate CNet transport profile; CHttp does not silently select it or fall back to another provider. |
| WSS over the same profiles | **not yet verified by this matrix** | WSS reuses CHttp/CNet TLS, but requires its own real WebSocket product-path evidence. |

The exact-source P-384 lane records immutable CHttp, Salts, SaltsUtils and
provider-registry commits and runs the HTTPS executable under ASan/UBSan.
Ordinary native SDK jobs cover build/install and the established RSA/P-256
tests on desktop platforms; Android/iOS build lanes are not device-runtime TLS
evidence. Published-package or cross-platform status must quote the matching
SDK manifests rather than infer provenance from a semantic version range.
