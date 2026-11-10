# Historical NuGet packaging

This directory preserves the old CHttp.Native package layout. Its floating
monolithic dependency restore and publication workflow have been retired.
Current builds use the [split SDK CI](../../.github/workflows/ci.yml) and
[explicit release inputs](../../cmake/BUILD.md). This migration does not publish
a new NuGet package. The layout below is historical, not a current build entry.

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

    find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)

For generated HTTP services and server-rendered pages, link `CHttp::App`
and include `<chttp_app/app.h>`. Both implementations are in `chttp_app`.
Rebuild former Service/Web consumers and deploy the new shared library into
a clean SDK prefix; the old targets, headers and DLLs are no longer installed.
