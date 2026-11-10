# Build with the split SDKs

## Dependency inputs

Use the exact versions and root variables in [`.env.example`](../.env.example).
CMake reads the parent environment and does not parse `.env`. Load it with your
existing environment manager, preserving credentials and unrelated settings.
Use absolute forward-slash paths. Only the matching vcpkg profile belongs in
`CMAKE_PREFIX_PATH`; first-party packages use their own configured roots.

The vcpkg-cache checkout must match `v<VCPKG_CACHE_VERSION>`. Restore its
published dependency tree through `scripts/restore-native-dependency.py`:
`resolve-release` resolves the producer triplet lock, then `locked` restores
that generated URL/SHA-256 lock. Use the payload root returned in the receipt.
Product presets consume preinstalled packages without an outer toolchain.

First-party SDKs come from published release manifests. Verify repository,
package/version, platform, producer CI, asset size and SHA-256 before extracting
the original archive into a fresh directory. `.github/ci/sdk-inputs.py` selects
the CMeta/Core/Utils release assets and uses vcpkg-cache download transport.
CI reuses the immutable CMeta entry point for Base/cache/compiler/host tools.
Missing inputs stop before product configuration; no alternate feed, SDK root,
toolchain, source build or disabled module is selected after a failure.

The [migration record](../docs/SPLIT_SDK_MIGRATION.md) documents the current
Utils packaging and Crypto API gaps. These releases cannot yet build CHttp.

## Configure, build, test and install

After loading the environment, on Windows:

```powershell
cmake/WithVs.cmd cmake --fresh --preset native-release-user
cmake/WithVs.cmd cmake --build --preset native-release-user
cmake/WithVs.cmd ctest --preset native-release-user --output-on-failure --no-tests=error
cmake/WithVs.cmd cmake --build --preset install-native-release-user
```

`VSDEVCMD_BAT` selects the developer environment; `LLVM_MINGW_ROOT` selects the
compiler SDK. Linux/macOS use `llvm-native-release-user`; Linux GCC uses
`gcc-release-user`, directly without the Windows wrapper. Each module owns its
tests. CI builds the full configured graph without selecting targets and runs
native tests through CTest. A failed configure/build stops that profile.

Use `--fresh` explicitly after changing SDK/compiler/triplet inputs. Ordinary
builds do not clear caches or repair dependency roots. The install preset writes
to its own stage, never into a dependency root.

## Android

Target SDKs and dependencies must match the selected ABI. IDL generation uses
a native host executable through `SaltsUtils_IDLC_EXECUTABLE`, with explicit
`SaltsUtils_IDLC_HOST_SALTS_BASE_ROOT` and `SaltsUtils_IDLC_HOST_CMETA_ROOT`.
CI restores a separate released Linux host compiler closure. Cross-build and
installation do not prove device execution; CI has no Android device runner.

## CI variables

Set `VCPKG_CACHE_VERSION`, `SALTS_BASE_VERSION`, `CMETA_VERSION`,
`SALTS_CORE_VERSION`, and `SALTS_UTILS_VERSION` to exact stable releases.
`CMETA_REF` selects the immutable preparation entry point, currently
`a5fad1682fcf501160b8fc6662dd55724d7b740d`.

The eight published SDK profiles are explicit in
[`sdk-matrix.json`](../.github/ci/sdk-matrix.json). The prepare job downloads
inputs once. Workers verify the same blobs, build, run native CTests and install
SDK archives with dependency provenance. Failed jobs remain visible; no retry
or source-built SDK fallback is part of this workflow.
