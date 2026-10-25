# CHttp X.509 compatibility matrix

This matrix describes evidence through CHttp's real HTTPS client and server
APIs. Primitive support or a CNet-only handshake is not promoted to a CHttp
claim. CHttp remains provider-neutral and links no OpenSSL, BoringSSL, or
GmSSL API directly; TLS and certificate validation belong to CNet.

| Certificate profile or condition | CHttp status | Executable evidence and limit |
| --- | --- | --- |
| RSA-2048/3072 leaf, RSA issuer, SHA-256 signature | **verified** | `chttp_tls_test` performs HTTPS requests through the real client/server path. The exact-source matrix adds RSA-3072 server authentication while the baseline covers middleware, sessions, connection reuse, peer certificate identity and required RSA-2048 mTLS. |
| P-256 client or server identity, ECDSA/SHA-256 | **verified** | The baseline requires a P-256 mTLS client identity; the exact-source matrix additionally completes server-authenticated HTTPS with the CNet P-256 fixture. |
| P-384 issuer, P-256 server leaf, ECDSA/SHA-384 | **verified on the exact-source Linux gate** | The opt-in `CHTTP_P384_SHA384_ACCEPTANCE_TESTS` case completes a real HTTPS request and rejects wrong CA, wrong DNS name, expired leaf and tampered certificate signature before middleware dispatch. |
| Hostname and trust-anchor mismatch | **verified** | Both failures are exercised against a running HTTPS server; the middleware count remains unchanged. |
| Expired leaf and invalid certificate signature | **verified for the P-384/P-256 profile** | Fixed independently parseable fixtures reach the TLS handshake but never HTTP middleware. |
| TLS 1.2/1.3, negotiated cipher and signature-scheme reporting | **not yet verified in CHttp** | CNet owns negotiation; the CHttp test does not currently expose or record the negotiated tuple. |
| RSA-PSS certificate signatures, SHA-512, P-521 issuer and Ed25519 | **unsupported by the pinned provider, verified fail-closed** | Independently validated Salts fixtures reach the real CHttp TLS boundary; server/profile creation or handshake fails with zero middleware dispatch and no provider fallback. TLS 1.3 RSA-PSS `CertificateVerify` remains distinct and supported. |
| Root/intermediate/leaf with omitted intermediate | **verified** | The full P-384 chain completes HTTPS; presenting the same leaf without its intermediate is rejected with zero middleware dispatch. |
| Invalid CA basic constraints/key usage | **verified** | A validly signed leaf plus an intermediate carrying `CA:FALSE` and no `keyCertSign` is rejected before HTTP dispatch. |
| Not-yet-valid leaf and invalid leaf EKU | **not yet verified in CHttp** | Flowie product lanes cover these conditions, but CHttp still needs fixed direct-product fixtures before claiming them here. |
| SM2/SM3 and TLCP | **unsupported by the CHttp HTTPS profile** | ShangMi TLS/TLCP qualification is a separate CNet transport profile; CHttp does not silently select it or fall back to another provider. |
| WSS over the same profiles | **not yet verified by this matrix** | WSS reuses CHttp/CNet TLS, but requires its own real WebSocket product-path evidence. |

The exact-source lane records immutable CHttp, Salts, SaltsUtils and
provider-registry commits and runs the HTTPS executable under ASan/UBSan. Its
extended matrix consumes the exact Salts fixture directory rather than copying
provider material or exposing provider types through CHttp.
Ordinary native SDK jobs cover build/install and the established RSA/P-256
tests on desktop platforms; Android/iOS build lanes are not device-runtime TLS
evidence. Published-package or cross-platform status must quote the matching
SDK manifests rather than infer provenance from a semantic version range.
