# Split SDK migration handoff

- Socket policy audit: HTTP async/blocking Client, Server accepted streams and
  standalone WebSocket already forward CNet options. H2 WebSocket Pool omitted
  both validation and application of `config.client.socket_options`; apply it
  after CNet initialization and before Manager/connection admission. Omitted
  policy remains unchanged. The module-owned socket test reads NODELAY and
  KEEPALIVE from the real H2 socket through CNet; two sessions share that policy.
  Original failures are in `build/migration-websocket-socket-red-fixture-ctest.log`;
  the fixed focused regression passes in `build/migration-websocket-socket-fixed-ctest.log`.
  Full published-SDK Windows build and CTest pass 104/104, recorded in
  `build/migration-websocket-socket-full-ctest.log`. ManagedDial/recovery remains
  separate #231 work: socket option forwarding does not authorize HTTP replay
  or prove protocol READY/old-request settlement.

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
  requirements resolves `generator_conformance`. The pool test now checks
  accepted cancellation through the write deadline and the eventual timeout
  plus resource release, matching CNet's observed-completion contract.
  Core 0.1.4 is restored from its published, verified SDK. The latest full build
  passes. The initial 102/103 result and H2 TLS failure are retained in
  `build/migration-crypto-final-full-ctest.log`.
- H2 TLS root cause was the fixture's thread ownership. It initialized the
  server CNet client on main but sent retained responses on its worker. CNet
  rejected the send with SALTS_EPERM. Assigning the poll return into the same
  status then hid that callback failure behind a later timeout. Initialize
  the client on the worker and preserve the callback's first error. Drain
  and join the worker before response assertions so a failed request cannot
  leave a thread borrowing its stack fixture. Published Core 0.1.4 now passes
  the complete Windows suite, 103/103, recorded in
  `build/migration-h2-owner-published-ctest.log`.
- Diagnosis used a temporary Core v0.1.4 source worktree with TLS size/status
  traces through the existing CHttp CTest. Product configuration was restored
  to the published SDK before final validation. Temporary source changes,
  worktree and investigation branches are removed after retaining their logs.
- Local dotenv credentials yielded HTTP 401 during restoration. The local task
  helper explicitly selected the existing gh login and preserved credentials.
  Committed CI uses its job token and the shared verified archive transport.
- `.env` now names the actual published SDKs restored into versioned roots.
  Its unrelated fields were compared and preserved. CMake does not load dotenv.
  Keep original failure logs and do not retry unchanged CI jobs.
- #231 READY qualification: `chttp_pool_lifecycle_test` now withholds the real
  H2 receive completion while continuing other NativeIO routing. TCP CONNECTED
  alone leaves the pool CONNECTING; consumed peer SETTINGS publishes READY.
  A pre-SETTINGS cancellation remains one ECANCELED result after late bytes,
  with its old request handle retired and no additional physical connection.
  The fixture matches native request slot/generation, owns the delayed
  completion through delivery/teardown and closes H2 as well as H1 before
  external stop. No product hook, ManagedDial or retry API was introduced.
  Published-SDK Windows build and full CTest pass 104/104; evidence:
  `build/migration-h2-ready-full-ctest.log`. #231 remains open.
- CI 42947933660 reported five TSan races in the io_uring WebSocket fixture:
  session capture was release-published but read without acquire, and JWT
  subject publication used a relaxed counter. Acquire the captured session,
  retire each connection before resetting its publication flag, and publish
  the subject with release/acquire. The published-SDK Windows build and
  WebSocket CTests pass; Linux TSan validation requires the new CI run.
