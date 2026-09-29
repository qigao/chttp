# CHttp.Native

Prebuilt Release SDKs for qigao/chttp.

`CHttp.Native` contains the prebuilt CHttp SDK only. Consumers restore these SDKs explicitly:

- `Salts.Native`
- `SaltsUtils.Native`

## Layout

- `sdk/linux-x64/`
- `sdk/windows-x64/`
- `sdk/macos-x64/` or `sdk/macos-arm64/`
- `sdk/android-arm64-v8a/`

Android arm64-v8a requires API 26 or newer, matching the selected `SaltsUtils.Native` SDK.

Consumers restore the package graph, set `SALTS_ROOT`, `SALTS_UTILS_ROOT`, and
`CHTTP_ROOT` to the matching platform directories, then use:

    find_package(Chttp CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)
