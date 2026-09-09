# Vendored libecc

- Source: https://github.com/libecc/libecc
- Commit: `6e8f214f41f65d5f30b04da75472f9c24f2100db`
- Commit date: 2026-07-17
- License: dual BSD/GPLv2; TurboNet redistributes and uses it under the BSD
  terms in `LICENSE`.

The upstream files in this directory are an unmodified source snapshot
produced with `git archive`; this `UPSTREAM.md` file records local provenance.
`../CMakeLists.txt` provides the integration and limits the build to the WEI448
curve, SHAKE256 hash, and EDDSA448 signature implementation.
