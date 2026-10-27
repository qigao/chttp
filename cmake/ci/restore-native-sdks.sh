#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
rid="${1:?target RID is required}"
with_turbowasm="${2:-false}"
output_format="${3:-github}"
case "$with_turbowasm" in true|false) ;; *) echo "with-turbowasm must be true or false" >&2; exit 1 ;; esac
case "$output_format" in
  github) : "${GITHUB_ENV:?GITHUB_ENV is required}" ;;
  json) ;;
  *) echo "output format must be github or json" >&2; exit 1 ;;
esac
case "$rid" in
  windows-x64|linux-x64|macos-arm64|macos-x64|android-arm64-v8a|ios-arm64) ;;
  *) echo "unsupported SDK RID: $rid" >&2; exit 1 ;;
esac
windows_host=false
python_command=python3
case "$(uname -s)" in
  MINGW*|MSYS*) windows_host=true; python_command=python ;;
esac
# Filesystem operations use Git Bash paths; native tools and CMake use C:/ paths.
native_path() {
  if [[ "$windows_host" == true ]]; then cygpath -am -- "$1"; else printf '%s\n' "$1"; fi
}
restore_root="${SDK_RESTORE_ROOT:-${RUNNER_TEMP:?SDK_RESTORE_ROOT or RUNNER_TEMP is required}}"
if [[ "$windows_host" == true ]]; then restore_root="$(cygpath -u -- "$restore_root")"; fi
if [[ "$restore_root" != /* || "$restore_root" == / ]]; then
  echo "SDK restore directory must be an absolute non-root directory" >&2
  exit 1
fi
mkdir -p -- "$restore_root"
restore_root="$(cd -- "$restore_root" && pwd -P)"
if [[ "$restore_root" == / ]] || [[ "$windows_host" == true && "$(native_path "$restore_root")" =~ ^[A-Za-z]:/$ ]]; then
  echo "SDK restore directory must not be a filesystem root" >&2
  exit 1
fi
packages="${QIGAO_NUGET_PACKAGES:-$restore_root/qigao-nuget}"
if [[ "$windows_host" == true ]]; then packages="$(cygpath -u -- "$packages")"; fi
mkdir -p -- "$packages"
packages="$(native_path "$(cd -- "$packages" && pwd -P)")"
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
curl_client="$repository_root/build/vcpkg-cache-contract/scripts/github-packages.py"
if [[ ! -f "$curl_client" ]]; then
  echo "Missing immutable shared curl NuGet V3 client: $curl_client" >&2
  exit 1
fi
export GH_TOKEN="${GH_TOKEN:-$GITHUB_TOKEN}"
salts_version='2.3.0-rc.16'
utils_version='4.3.0-rc.11'
wasm_version=''

restore_exact_package() {
  local id="$1" version="$2"
  local identity="${id,,}"
  local archive="$restore_root/$identity.$version.nupkg"
  local folder="$packages/$identity/$version"
  "$python_command" "$(native_path "$curl_client")" package "$id" "$version" \
    "$(native_path "$archive")" >&2
  "$python_command" "$(native_path "$curl_client")" extract \
    "$(native_path "$archive")" "$(native_path "$folder")" >&2
}
if [[ -z "${SALTS_CANDIDATE_ROOT:-}" ]]; then
  restore_exact_package 'Salts.Native' "$salts_version"
fi
restore_exact_package 'SaltsUtils.Native' "$utils_version"
if [[ "$with_turbowasm" == true ]]; then
  version_file="$restore_root/turbowasm.version"
  wasm_archive="$restore_root/turbowasm.latest.nupkg"
  "$python_command" "$(native_path "$curl_client")" latest \
    'TurboWasm.Native' "$(native_path "$wasm_archive")" \
    "$(native_path "$version_file")" >&2
  wasm_version="$(tr -d '\r\n' < "$version_file")"
  if [[ ! "$wasm_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
    echo "Unexpected non-stable TurboWasm.Native selection: $wasm_version" >&2
    exit 1
  fi
  "$python_command" "$(native_path "$curl_client")" extract \
    "$(native_path "$wasm_archive")" \
    "$(native_path "$packages/turbowasm.native/$wasm_version")" >&2
fi
candidate_root="${SALTS_CANDIDATE_ROOT:-}"
if [[ -n "$candidate_root" ]]; then candidate_root="$(native_path "$candidate_root")"; fi
github_env=""
if [[ "$output_format" == github ]]; then github_env="$(native_path "$GITHUB_ENV")"; fi
# Commit roots only after the complete requested SDK graph has been checked.
"$python_command" - "$packages" \
  "$rid" "$candidate_root" "$with_turbowasm" "$output_format" "$github_env" \
  "$salts_version" "$utils_version" "$wasm_version" <<'PY'
import json, os, pathlib, sys
packages, rid, candidate, with_wasm, output_format, github_env, salts_version, utils_version, wasm_version = sys.argv[1:]
requested = {"salts.native": salts_version, "saltsutils.native": utils_version,
             "turbowasm.native": wasm_version}
if salts_version != "2.3.0-rc.16" or utils_version != "4.3.0-rc.11":
    raise SystemExit("Published Salts/SaltsUtils native SDK version contract changed")

def sdk_root(package):
    identity = package.lower()
    version = requested.get(identity)
    if not version:
        raise SystemExit(f"No exact published GitHub Packages version for {package}")
    print(f"curl verified {package}/{version} (RID={rid})", file=sys.stderr)
    return (pathlib.Path(packages) / identity / version / 'sdk' / rid).resolve()

salts = pathlib.Path(candidate).resolve() if candidate else sdk_root('Salts.Native')
utils = sdk_root('SaltsUtils.Native')
native_binding = utils / 'include/data_bind_native_binding.h'
required = [salts / 'lib/cmake/Salts/SaltsConfig.cmake', salts / 'include/cmeta/function.h',
            utils / 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake',
            utils / 'include/data_bind_method_plan.h', native_binding]
roots = dict(SALTS_ROOT=salts.as_posix(), SALTS_UTILS_ROOT=utils.as_posix(),
             QIGAO_NUGET_PACKAGES=packages)
libraries = [utils / 'lib', utils / 'bin', salts / 'lib']
if with_wasm == 'true':
    wasm = sdk_root('TurboWasm.Native')
    roots['TURBOWASM_ROOT'] = wasm.as_posix()
    required += [wasm / 'lib/cmake/TurboWasm/TurboWasmConfig.cmake', wasm / 'include/turbowasm/component.h']
    libraries.append(wasm / 'lib')
for path in required:
    if not path.is_file():
        raise SystemExit(f'Restored SDK lacks required RID={rid} file: {path}')
if 'DataBindNativeExecution' not in native_binding.read_text(encoding='utf-8'):
    raise SystemExit(f'SaltsUtils SDK lacks DataBindNativeExecution (RID={rid})')
if os.name != 'nt':
    roots['LD_LIBRARY_PATH'] = ':'.join([p.as_posix() for p in libraries] + [os.environ.get('LD_LIBRARY_PATH', '')])
if any('\n' in value or '\r' in value for value in roots.values()):
    raise SystemExit('SDK paths must not contain newlines')
if output_format == 'json':
    print(json.dumps(roots))
else:
    with open(github_env, 'a', encoding='utf-8', newline='\n') as output:
        output.writelines(f'{key}={value}\n' for key, value in roots.items())
PY
