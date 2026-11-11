# Split SDK migration handoff

- The existing working tree contained the shared-presets migration. It was
  preserved and extended; see `docs/SPLIT_SDK_MIGRATION.md` for dependency
  ownership, the explicit eight-profile matrix and rollback.
- Remote master advanced to `6c8567a350c91701cee276683c972376814417a5` during
  migration. Its HTTPS P-384/SHA-384/X.509 runtime regressions are preserved;
  22 fixed PEM inputs are module-owned and the cases run with BUILD_TESTS.
  The old exact-source CI is superseded, not the security regression coverage.
- Actual Windows LLVM-MinGW configure against published Base 0.1.4, CMeta 0.1.4,
  CNet 0.1.3 and Utils 0.1.0 fails because Utils omits required `IDL.cmake`.
  Original log: `build/migration-split-sdk-initial-configure.log`. CHttp build
  and runtime tests have not passed; do not report this migration complete.
- Utils packaging fix: qigao/salts-utils PR 755, branch `fix/sdk-idl-install`,
  commit `32608153766f255cca76b36b14e5d504a626f9e0`. Local product configure,
  build and installation succeeded into `salts-utils/build/sdk-idl-install-stage`;
  all 11 compiler templates and IDL.cmake are installed. Existing native CTests
  passed 188/188. Published Utils 0.1.1 now supplies the packaging fix. Its
  manifest records SDK profile, archive digest and producer run using the
  Utils release schema. CHttp configuration, the HTTP example IDL generation
  target and the thread CPU CTest passed after switching to the release and
  fixing the missing Ed25519 fixture and the `Salts::URI` target name.
- `vendor/cjwt` owns JWT/JWE and now builds its prior GmSSL/libecc primitives
  privately. The old SDK header dependency is removed. HTTP/S3 use the internal
  `chttp_digest` target. Full Windows LLVM-MinGW build and install pass, as do
  all 11 cjwt CTests and HTTP JWT/CSRF/WebSocket/S3 tests. Source provenance,
  license, ownership and failure contracts are in `vendor/cjwt/crypto/README.md`.
- Full CTest initially passed 100/103. Installing the existing pinned OpenAPI
  Python requirements resolves `generator_conformance`. Pool write-timeout and
  H2-over-TLS tests still fail with Core 0.1.3. Keep the original failures in
  `build/migration-crypto-private-full-ctest.log`; CHttp remains draft pending
  complete qualification.
- Local dotenv credentials yielded HTTP 401 during restoration. The local task
  helper explicitly selected the existing gh login and preserved credentials.
  Committed CI uses its job token and the shared verified archive transport.
- `.env` now names the actual published SDKs restored into versioned roots.
  Its unrelated fields were compared and preserved. CMake does not load dotenv.
  Keep original failure logs and do not retry unchanged CI jobs.
