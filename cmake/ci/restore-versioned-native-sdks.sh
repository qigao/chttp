#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${GITHUB_ENV:?GITHUB_ENV is required}"

rid="${1:?target RID is required}"
salts_version="${SALTS_SDK_VERSION:-1.1.0}"
utils_version="${SALTS_UTILS_SDK_VERSION:-2.0.0}"
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
    <PackageReference Include="Salts.Native" Version="[$salts_version]" />
    <PackageReference Include="SaltsUtils.Native" Version="[$utils_version]" />
  </ItemGroup>
</Project>
EOF
dotnet restore "$project" --packages "$packages" --configfile "$config" --no-cache

salts_root="$packages/salts.native/$salts_version/sdk/$rid"
utils_root="$packages/saltsutils.native/$utils_version/sdk/$rid"
test -f "$salts_root/lib/cmake/Salts/SaltsConfig.cmake"
test -f "$utils_root/lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"
printf "SALTS_ROOT=%s\n" "$salts_root" >> "$GITHUB_ENV"
printf "SALTS_UTILS_ROOT=%s\n" "$utils_root" >> "$GITHUB_ENV"
printf "QIGAO_NUGET_PACKAGES=%s\n" "$packages" >> "$GITHUB_ENV"
