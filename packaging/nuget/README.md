# CHttp.Native

Prebuilt Release SDKs for qigao/chttp.

`2.1.0-rc.1` is the prepared prerelease. CMake library/package compatibility
uses numeric `2.1.0`; the installed config exposes `CHTTP_RELEASE_VERSION`
with the full prerelease identifier. NuGet metadata and the release tag use
`2.1.0-rc.1` and `v2.1.0-rc.1` respectively.

Manual `native-sdk-release.yml` runs build, test, install and pack without
publishing. Only a matching pushed version tag enables publication; existing
package versions are not silently skipped or replaced.

`CHttp.Native` contains the prebuilt CHttp SDK only. Consumers restore these SDKs explicitly:

- `Salts.Native`
- `SaltsUtils.Native`

The current integration resolves `Salts.Native 2.3.0-*` and
`SaltsUtils.Native 4.3.0-*` on each run. Record both resolved SDK versions and
commits from their manifests; rebuild consumers when moving to this SDK family.
TLS and protocol crypto use the Salts GmSSL-backed APIs, so consumers do not
add OpenSSL/BoringSSL or a public GmSSL CMake dependency.

## Layout

- `sdk/linux-x64/`
- `sdk/windows-x64/`
- `sdk/macos-x64/` or `sdk/macos-arm64/`
- `sdk/android-arm64-v8a/`
- `sdk/ios-arm64/`

Android arm64-v8a requires API 26 or newer, matching the selected `SaltsUtils.Native` SDK.
iOS packaging includes device arm64 only, matching the latest dependency SDKs;
the simulator target is not published.

Consumers restore the package graph, set `SALTS_ROOT`, `SALTS_UTILS_ROOT`, and
`CHTTP_ROOT` to the matching platform directories, then use:

    file(TO_CMAKE_PATH "$ENV{CHTTP_ROOT}" CHTTP_ROOT_PATH)
    find_package(Chttp CONFIG REQUIRED PATHS "${CHTTP_ROOT_PATH}" NO_DEFAULT_PATH)

For generated HTTP services and server-rendered pages, link `CHttp::App`
and include `<chttp_app/app.h>`. Both implementations are in `chttp_app`.
Rebuild former Service/Web consumers and deploy the new shared library into
a clean SDK prefix; the old targets, headers and DLLs are no longer installed.
