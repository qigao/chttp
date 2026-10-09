#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${GITHUB_ENV:?GITHUB_ENV is required}"

rid="${1:?target RID is required}"
with_turbowasm="${2:-false}"
case "$with_turbowasm" in true|false) ;; *) echo "with-turbowasm must be true or false" >&2; exit 1 ;; esac
packages="${QIGAO_NUGET_PACKAGES:-$RUNNER_TEMP/qigao-nuget}"
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
config="$repository_root/cmake/vcpkg-cache.nuget.config"
project="$RUNNER_TEMP/qigao-chttp-sdk-restore.csproj"

cat > "$project" <<'EOF'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="2.3.0-*" Condition="'$(UseSaltsCandidate)' != 'true'" />
    <PackageReference Include="SaltsUtils.Native" Version="4.3.0-*" />
    <PackageReference Include="TurboWasm.Native" Version="*" Condition="'$(WithTurboWasm)' == 'true'" />
  </ItemGroup>
</Project>
EOF
use_candidate=false
if [ -n "${SALTS_CANDIDATE_ROOT:-}" ]; then use_candidate=true; fi
dotnet restore "$project" --packages "$packages" --configfile "$config" --no-cache --force-evaluate -p:WithTurboWasm="$with_turbowasm" -p:UseSaltsCandidate="$use_candidate"

sdk_root() {
  python3 - "$RUNNER_TEMP/obj/project.assets.json" "$packages" "$1" "$rid" <<'PY'
import json, pathlib, sys
assets = json.loads(pathlib.Path(sys.argv[1]).read_text())
matches = [v['path'] for k, v in assets['libraries'].items()
           if k.lower().startswith(sys.argv[3].lower() + '/')]
if len(matches) != 1:
    raise SystemExit('expected one resolved ' + sys.argv[3] + ' package')
print(f'Resolved {sys.argv[3]}: {matches[0]} (RID={sys.argv[4]})', file=sys.stderr)
print((pathlib.Path(sys.argv[2]) / matches[0] / 'sdk' / sys.argv[4]).resolve())
PY
}

if [ "$use_candidate" = true ]; then
  salts_root="$SALTS_CANDIDATE_ROOT"
else
  salts_root="$(sdk_root salts.native)"
fi
utils_root="$(sdk_root saltsutils.native)"
printf 'Resolved SDK roots (RID=%s): SALTS_ROOT=%s SALTS_UTILS_ROOT=%s\n' \
  "$rid" "$salts_root" "$utils_root"
for required in \
  "$salts_root/lib/cmake/Salts/SaltsConfig.cmake" \
  "$salts_root/include/cmeta/function.h" \
  "$utils_root/lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake" \
  "$utils_root/include/data_bind_method_plan.h" \
  "$utils_root/include/data_bind_native_binding.h"; do
  if [ ! -f "$required" ]; then
    echo "Restored prerelease SDK lacks required RID=$rid file: $required" >&2
    exit 1
  fi
done
grep -q "DataBindNativeExecution" "$utils_root/include/data_bind_native_binding.h" || {
  echo "SaltsUtils SDK lacks DataBindNativeExecution (RID=$rid)" >&2
  exit 1
}
printf "SALTS_ROOT=%s\n" "$salts_root" >> "$GITHUB_ENV"
printf "SALTS_UTILS_ROOT=%s\n" "$utils_root" >> "$GITHUB_ENV"
printf "QIGAO_NUGET_PACKAGES=%s\n" "$packages" >> "$GITHUB_ENV"

if [ "$with_turbowasm" = true ]; then
  turbowasm_root="$(sdk_root turbowasm.native)"
  test -f "$turbowasm_root/lib/cmake/TurboWasm/TurboWasmConfig.cmake"
  test -f "$turbowasm_root/include/turbowasm/component.h"
  printf "TURBOWASM_ROOT=%s\n" "$turbowasm_root" >> "$GITHUB_ENV"
  printf "LD_LIBRARY_PATH=%s/lib:%s/bin:%s/lib:%s/lib:%s\n" \
    "$utils_root" "$utils_root" "$salts_root" "$turbowasm_root" "${LD_LIBRARY_PATH:-}" >> "$GITHUB_ENV"
else
  printf "LD_LIBRARY_PATH=%s/lib:%s/bin:%s/lib:%s\n" \
    "$utils_root" "$utils_root" "$salts_root" "${LD_LIBRARY_PATH:-}" >> "$GITHUB_ENV"
fi
