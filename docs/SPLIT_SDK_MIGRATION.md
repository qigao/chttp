# Split SDK migration

## Decision

The former Salts/SaltsUtils prerelease composition mixed platform primitives,
metadata, transport and utility ownership. CHttp now selects installed
SaltsBase, CMeta, SaltsCore (from salts-cnet) and SaltsUtils releases explicitly.
Their versions and roots come from the environment, with matching local dotenv
examples and repository CI variables. `VCPKG_CACHE_VERSION` selects the shared
infrastructure and published dependency trees; product configuration performs
no downloads or dependency repair.

The alternatives were retaining the floating monolithic NuGet families or
building upstream sources whenever a published package was absent. Both obscure
the release composition and are rejected. A missing package or API remains a
failure at the owning boundary.

## Module and interface boundaries

- SaltsBase owns Core clocks, threads, files, buffers, NativeIO and parsers.
- CMeta owns `CMeta::CMeta`, `CMeta::CFlow`, CSTL and plugin targets.
- SaltsCore owns `Salts::CNet` and `Salts::Crypto`; its repository is salts-cnet.
- SaltsUtils owns DataBind, IDL, bindings and Jinja targets under `Salts::`.
- CHttp retains request/response, HTTP/RPC/S3, authentication policy, connection
  placement, cancellation and stop/drain contracts. This migration must not
  change their public layouts, wire formats, TLS verification or error mapping.

Old foundation header paths and `cmeta_` platform symbols are changed at their
callers. There are no compatibility alias headers or replacement runtimes.
UUID callers use Base's 16-byte `uuid_t` array and pass the array directly;
ownership and textual output remain with the existing callers.

Tests remain under their owner modules and register through each module's
`BUILD_TESTS` block. Shared HTTP integration tests belong to http_common.
TinyTest itself is unchanged. Installed-consumer projects and source-text
contract assertions are not verification entry points for this migration.
The superseded floating NuGet, source-candidate and installed-consumer workflows
are retired in favor of the single split-SDK matrix. Runtime test and benchmark
sources remain in their owning modules.
The main-branch P-384/SHA-384 and X.509 HTTPS tests are preserved and always
included in `BUILD_TESTS`, without a test-subset option. Their 22 fixed PEM
inputs now belong to `http_client/tests/fixtures`, with the immutable original
source recorded there. The historical X.509 qualification document distinguishes
earlier monolithic results from the still-unqualified split SDK composition.

## CI composition

The matrix is explicit, not computed by silently filtering missing assets.
Eight declared profiles match all four released SDK sets. Linux ARM64 cannot
join until CNet publishes matching SDKs; Windows ARM64 and macOS x64 also lack
the required complete composition. Native profiles build the complete graph,
run CTest and install. Android profiles cross-build and install; no device
execution is claimed. Android IDL generation uses the released Linux host
Utils compiler and its separately restored Base/CMeta dependencies.

The prepare job reuses the immutable CMeta input preparation entry point for
Base/cache/compiler archives and the vcpkg-cache verified transport for the
remaining published SDKs. Matrix workers use the same original archive bytes
and digest-qualified manifests. Restoration emits receipts with version,
source commit, producer, platform, digest and root. SDK assets are never amended
in place. A source SDK build, alternate feed, guessed package root, disabled
module or retry is not a recovery step.

## Upstream gaps and validation

The first Windows LLVM-MinGW configure used Base 0.1.4, CMeta 0.1.4,
CNet/SaltsCore 0.1.3, Utils 0.1.0 and vcpkg-cache 0.1.3. Release manifests,
archive sizes and SHA-256 digests were verified before extraction.

Utils 0.1.1 ships `IDL.cmake` and the compiler templates that were missing in
0.1.0. The preparation job validates the published Utils manifest against its
release tag and successful producer CI, checks the SDK source trees, and
verifies each archive against its digest and GitHub asset size.
Windows LLVM-MinGW configuration passed with this SDK after restoring the
missing Ed25519 test key and selecting Base's exported `Salts::URI` target.
The existing `http_example_contract_idl_codegen` target and
`chttp_thread_cpu_test` passed.

**HIGH:** `vendor/cjwt` owns JWT/JWE and currently calls the old JOSE primitive
interface (`salts/crypto.h`). HTTP and S3 also use the old digest helpers
(`cmeta_crypto.h`). The full Windows build stops at the missing cjwt include.
Required operations include HMAC, SHA-1/SHA-256,
key import, RSA/ECDSA/EdDSA signing and verification, OAEP, AES-GCM, key wrap,
PBKDF2, secure clearing and constant-time comparison. CNet's published SHA-256
and TLS interfaces do not supply that entire API. Existing
JWT/JWE/S3/WebSocket tests must qualify the completed composition.

Local restoration initially stopped on HTTP 401 from dotenv GitHub credentials.
For this task, restoration explicitly selected the already authenticated gh
login. Existing dotenv credentials were preserved. CI uses its job token;
there is no authentication fallback in the committed restoration code.

## Migration and rollback

Restore new releases into version/profile-specific directories, update only
the dependency fields in the local environment, and configure with `--fresh`
to remove stale compiler/package caches. Preserve unrelated machine settings
and credentials. Run the existing user preset's full build and formal CTests
before installation and delivery. A successful configure alone proves neither
ABI compatibility nor protocol behavior.

Rollback explicitly restores the previous code revision and its matching
environment/SDK pair, followed by a fresh configure and rebuild. Never mix an
old monolithic SDK into the new roots, alter an existing release archive, or
reuse binaries from an incompatible compiler/profile.
