# Split SDK migration handoff

- The existing working tree contained the shared-presets migration. It was
  preserved and extended; see `docs/SPLIT_SDK_MIGRATION.md` for dependency
  ownership, the explicit eight-profile matrix and rollback.
- Remote master advanced to `6c8567a350c91701cee276683c972376814417a5` during
  migration. Its HTTPS P-384/SHA-384/X.509 runtime regressions are preserved;
  21 fixed PEM inputs are module-owned and the cases run with BUILD_TESTS.
  The old exact-source CI is superseded, not the security regression coverage.
- Actual Windows LLVM-MinGW configure against published Base 0.1.4, CMeta 0.1.4,
  CNet 0.1.3 and Utils 0.1.0 fails because Utils omits required `IDL.cmake`.
  Original log: `build/migration-split-sdk-initial-configure.log`. CHttp build
  and runtime tests have not passed; do not report this migration complete.
- Utils packaging fix: qigao/salts-utils PR 755, branch `fix/sdk-idl-install`,
  commit `32608153766f255cca76b36b14e5d504a626f9e0`. Local product configure,
  build and installation succeeded into `salts-utils/build/sdk-idl-install-stage`;
  all 11 compiler templates and IDL.cmake are installed. Existing native CTests
  passed 188/188. No released SDK directory was modified. A new release needs
  the pending user's publication decision and successful producer CI.
- JOSE and digest APIs used by cjwt/JWT/S3/WebSocket are absent from the split
  Crypto SDK. A pending user question covers extending the public CNet Crypto
  API and publishing it. Do not copy old crypto implementations, bypass TLS or
  authentication, or silently disable these features.
- Local dotenv credentials yielded HTTP 401 during restoration. The local task
  helper explicitly selected the existing gh login and preserved credentials.
  Committed CI uses its job token and the shared verified archive transport.
- `.env` now names the actual published SDKs restored into versioned roots.
  Its unrelated fields were compared and preserved. CMake does not load dotenv.
  Keep original failure logs and do not retry unchanged CI jobs.
