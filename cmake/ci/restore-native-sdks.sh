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
config="$(native_path "$repository_root/cmake/vcpkg-cache.nuget.config")"
project="$restore_root/qigao-chttp-sdk-restore.csproj"

cat > "$project" <<'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[2.3.0-rc.16]" Condition="'$(UseSaltsCandidate)' != 'true'" />
    <PackageReference Include="SaltsUtils.Native" Version="[4.3.0-rc.11]" />
    <PackageReference Include="TurboWasm.Native" Version="*" Condition="'$(WithTurboWasm)' == 'true'" />
  </ItemGroup>
</Project>
EOF
use_candidate=false
if [ -n "${SALTS_CANDIDATE_ROOT:-}" ]; then use_candidate=true; fi
dotnet restore "$(native_path "$project")" --packages "$packages" --configfile "$config" \
  --no-cache --force-evaluate -p:WithTurboWasm="$with_turbowasm" -p:UseSaltsCandidate="$use_candidate" >&2

candidate_root="${SALTS_CANDIDATE_ROOT:-}"
if [[ -n "$candidate_root" ]]; then candidate_root="$(native_path "$candidate_root")"; fi
github_env=""
if [[ "$output_format" == github ]]; then github_env="$(native_path "$GITHUB_ENV")"; fi
# Commit roots only after the complete requested SDK graph has been checked.
"$python_command" - "$(native_path "$restore_root/obj/project.assets.json")" "$packages" \
  "$rid" "$candidate_root" "$with_turbowasm" "$output_format" "$github_env" <<'PY'
import json, os, pathlib, sys
assets = json.loads(pathlib.Path(sys.argv[1]).read_text())
packages, rid, candidate, with_wasm, output_format, github_env = sys.argv[2:]

def sdk_root(package):
    matches = [(k, v['path']) for k, v in assets['libraries'].items()
               if k.lower().startswith(package.lower() + '/')]
    if len(matches) != 1:
        raise SystemExit('expected one resolved ' + package + ' package')
    # Preserve the published SDK pair selected by the native release workflow.
    required_versions = {'salts.native': '2.3.0-rc.16',
                         'saltsutils.native': '4.3.0-rc.11'}
    want = required_versions.get(package.lower())
    if want and matches[0][0].lower() != package.lower() + '/' + want:
        raise SystemExit('unexpected native SDK release version: ' + matches[0][0])
    print(f'Resolved {package}: {matches[0][0]} (RID={rid})', file=sys.stderr)
    return (pathlib.Path(packages) / matches[0][1] / 'sdk' / rid).resolve()

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
