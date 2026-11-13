# Split SDK migration

## Decision

The former Salts/SaltsUtils prerelease composition mixed platform primitives,
metadata, transport and utility ownership. CHttp now selects installed
SaltsBase, CMeta, SaltsCNet and SaltsUtils SDKs explicitly.
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
- SaltsCNet owns `Salts::CNet` and `Salts::Crypto`; its repository is salts-cnet.
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

The missing JOSE interface is resolved by restoring cjwt's prior GmSSL/libecc
adapter inside `vendor/cjwt/crypto`. Symbols use the private `cjwt_crypto_`
namespace. The original commit, dependency/license decisions, ownership and
error contracts are recorded in that directory's README. HTTP and S3 use
`http_common/http/chttp_digest` for the existing digest and clearing operations.
Windows LLVM-MinGW builds the complete graph and installs the SDK successfully.
All 11 cjwt CTests, including the independent JWS/JWE vectors, pass. HTTP JWT,
CSRF, WebSocket handshake and S3 regressions also pass.

The first full CTest run passed 100 of 103 tests. OpenAPI conformance initially
lacked its declared Python requirements. After installing the existing pinned
requirements file, that formal test passed. CI installs the same requirements.
The pool lifecycle test expected an intermediate ENOENT that CNet now treats
as an accepted cancellation of an already-observed completion. It now advances
through the configured write deadline and verifies the retained buffer lease,
final timeout, absence of suffix writes and successful pool reuse. That test
passes. Core selection is aligned to published 0.1.4, with its release manifest
and SDK source trees verified.

The H2 TLS fixture initialized its CNet client on main and admitted retained
sends on a worker, violating CNet's owner-thread contract. The rejected send
returned SALTS_EPERM inside a callback. The fixture overwrote that error with
the successful poll result, then timed out. Client initialization and teardown
now run on the worker, and poll completion preserves the first callback error.
The test joins the worker before checking the response. The complete Windows
CTest run against the published SDK passes 103/103. Original failures remain in
`build/migration-crypto-final-full-ctest.log`, and the successful run is recorded
in `build/migration-h2-owner-published-ctest.log`.

Local restoration initially stopped on HTTP 401 from dotenv GitHub credentials.
For this task, restoration explicitly selected the already authenticated gh
login. Existing dotenv credentials were preserved. CI uses its job token;
there is no authentication fallback in the committed restoration code.

## Migration and rollback

CI 42947933660 identified five TSan reports in WebSocket test probes. The
session reader now acquires the callback's publication flag, each connection
retires before that flag is reused, and JWT subject publication uses matching
release/acquire ordering. Network response arrival alone does not synchronize
ordinary fixture memory. The Windows WebSocket CTests pass; the new Linux TSan
run remains the qualification for this synchronization fix.

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

## Plain CMake and current SDK composition

CMake resolves explicit SDK roots without upstream version arguments. Project
version metadata remains ordinary CMake package metadata; shared targets have no
VERSION/SOVERSION properties. GNUInstallDirs, exported targets and the existing
source/target helpers follow SaltsCNet's CMake layout. Installed consumers load
the selected upstream packages before Chttp; ChttpConfig only imports targets.

The selected composition is Base 0.1.8, SaltsCNet 0.1.12, Utils 0.1.4 and CMeta
0.1.6 at ref 679710f7e3312811efdd92494744a2499286a6a5. Utils declares the CMeta
source SDK build against Base 0.1.8; its release helper is the primary target/host
SDK flow. The old Base 0.1.6 CMeta binary is not part of this composition.
Restoration still verifies original release manifests, producer/tree identity,
asset size and SHA-256 before extraction. Product configuration performs no SDK
restoration, version selection, package-feed access or dependency repair.

Historical CHttp GitHub Releases and its CHttp.Native GitHub package were removed
at the maintainer's request. Product distribution uses CMake install trees.
No historical release workflow or NuGet publication is restored. Changes to SDK
filenames require a clean install prefix and downstream rebuild; rollback uses
an explicitly selected previous source/SDK composition.
