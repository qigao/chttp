#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${GITHUB_ENV:?GITHUB_ENV is required}"

rid="${1:?target RID is required}"
packages="${QIGAO_NUGET_PACKAGES:-$RUNNER_TEMP/qigao-nuget}"
config="$RUNNER_TEMP/qigao-nuget.config"
project="$RUNNER_TEMP/qigao-chttp-sdk-restore.csproj"

cat > "$config" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /></packageSources></configuration>
EOF
dotnet nuget add source https://nuget.pkg.github.com/qigao/index.json \
  --name github --username qigao --password "$GITHUB_TOKEN" \
  --store-password-in-clear-text --configfile "$config"
cat > "$project" <<EOF
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
    <PackageReference Include="SaltsUtils.Native" Version="*" />
  </ItemGroup>
</Project>
EOF
dotnet restore "$project" --packages "$packages" --configfile "$config" --no-cache

sdk_root() {
  local package="$1"
  local roots=()
  local root
  while IFS= read -r root; do
    roots+=("$root")
  done < <(
    find "$packages/$package" -mindepth 3 -maxdepth 3 -type d \
      -path "*/sdk/$rid" -print
  )
  if [ "${#roots[@]}" -ne 1 ]; then
    printf 'expected exactly one restored %s SDK for %s, found %s\n' \
      "$package" "$rid" "${#roots[@]}" >&2
    return 1
  fi
  printf '%s' "${roots[0]}"
}

salts_root="$(sdk_root salts.native)"
utils_root="$(sdk_root saltsutils.native)"
test -f "$salts_root/lib/cmake/Salts/SaltsConfig.cmake"
test -f "$salts_root/include/cmeta/function.h"
test -f "$utils_root/lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"
test -f "$utils_root/include/data_bind_method_plan.h"
test -f "$utils_root/include/data_bind_native_binding.h"
grep -q "DataBindNativeExecution" "$utils_root/include/data_bind_native_binding.h"
printf "SALTS_ROOT=%s\n" "$salts_root" >> "$GITHUB_ENV"
printf "SALTS_UTILS_ROOT=%s\n" "$utils_root" >> "$GITHUB_ENV"
printf "QIGAO_NUGET_PACKAGES=%s\n" "$packages" >> "$GITHUB_ENV"
