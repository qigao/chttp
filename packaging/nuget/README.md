# CHttp.Native

Versioned prebuilt Release SDKs for qigao/chttp.

`CHttp.Native 1.0.0` has exact native SDK dependencies:

- `Salts.Native 1.5.0`
- `SaltsUtils.Native 4.0.0`

## Layout

- `sdk/linux-x64/`
- `sdk/windows-x64/`
- `sdk/macos-x64/` or `sdk/macos-arm64/`
- `sdk/android-arm64-v8a/`

Android arm64-v8a requires API 26 or newer, matching `SaltsUtils.Native 4.0.0`.

Consumers restore the package graph, set `SALTS_ROOT`, `SALTS_UTILS_ROOT`, and
`CHTTP_ROOT` to the matching platform directories, then use:

    find_package(Chttp 1.0.0 EXACT CONFIG REQUIRED PATHS "$ENV{CHTTP_ROOT}" NO_DEFAULT_PATH)
