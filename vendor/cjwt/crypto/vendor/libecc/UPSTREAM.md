# Vendored libecc

- Source: https://github.com/libecc/libecc
- Commit: `6e8f214f41f65d5f30b04da75472f9c24f2100db`
- Commit date: 2026-07-17
- License: dual BSD/GPLv2; Salts redistributes and uses it under the BSD
  terms in `LICENSE`.

The selected upstream include and source files in this directory are
unmodified files from a snapshot produced with `git archive`; this
`UPSTREAM.md` records local provenance. `../../CMakeLists.txt` provides the
integration for the JOSE curve, hash and signature set. CHttp builds WEI448,
WEI25519, SECP256R1, SECP384R1, SECP521R1 and SECP256K1 with the required
SHAKE256/SHA-2 hashes and EdDSA/ECDSA implementations. SHA entry points are
namespaced in the consuming target to coexist with GmSSL.
