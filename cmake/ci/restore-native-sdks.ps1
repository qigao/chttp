param([Parameter(Mandatory=$true)][string]$Rid, [switch]$Local)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) { throw "GITHUB_TOKEN is required" }
if (-not $Local -and (-not $env:RUNNER_TEMP -or -not $env:GITHUB_ENV)) {
  throw "RUNNER_TEMP and GITHUB_ENV are required in CI"
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$restoreRoot = if ($Local) { Join-Path $repositoryRoot "build/native-sdk" } else { $env:RUNNER_TEMP }
$packages = if ($env:QIGAO_NUGET_PACKAGES) { $env:QIGAO_NUGET_PACKAGES } else { Join-Path $restoreRoot "qigao-nuget" }
$packages = [IO.Path]::GetFullPath($packages)
$config = Join-Path $repositoryRoot "cmake/vcpkg-cache.nuget.config"
$project = Join-Path $restoreRoot "qigao-chttp-sdk-restore.csproj"
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null
@'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <RestorePackagesWithLockFile>false</RestorePackagesWithLockFile>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
    <PackageReference Include="SaltsUtils.Native" Version="*" />
  </ItemGroup>
</Project>
'@ | Set-Content -LiteralPath $project
dotnet restore $project --packages $packages --configfile $config --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "failed to restore native SDKs" }
$assets = Get-Content -LiteralPath (Join-Path $restoreRoot "obj/project.assets.json") -Raw | ConvertFrom-Json -AsHashtable
function Get-RestoredSdkRoot([string]$packageName, [string]$rid) {
  $keys = @($assets.libraries.Keys | Where-Object { $_.StartsWith("$packageName/", [StringComparison]::OrdinalIgnoreCase) })
  if ($keys.Count -ne 1) { throw "expected one resolved $packageName package" }
  Write-Host "restored $($keys[0]) for $rid"
  return Join-Path (Join-Path $packages $assets.libraries[$keys[0]].path) "sdk/$rid"
}

$saltsRoot = Get-RestoredSdkRoot "salts.native" $Rid
$utilsRoot = Get-RestoredSdkRoot "saltsutils.native" $Rid
foreach ($p in @(
  (Join-Path $saltsRoot "lib\cmake\Salts\SaltsConfig.cmake"),
  (Join-Path $saltsRoot "include\cmeta\function.h"),
  (Join-Path $utilsRoot "lib\cmake\SaltsUtils\SaltsUtilsConfig.cmake"),
  (Join-Path $utilsRoot "include\data_bind_method_plan.h"),
  (Join-Path $utilsRoot "include\data_bind_native_binding.h")
)) {
  if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "missing restored SDK file: $p" }
}
$nativeBinding = Join-Path $utilsRoot "include\data_bind_native_binding.h"
if ((Get-Content -LiteralPath $nativeBinding -Raw) -notmatch "DataBindNativeExecution") {
  throw "restored SaltsUtils SDK does not publish DataBindNativeExecution"
}
$env:SALTS_ROOT = $saltsRoot
$env:SALTS_UTILS_ROOT = $utilsRoot
$env:QIGAO_NUGET_PACKAGES = $packages
if (-not $Local) {
  "SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
  "SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
  "QIGAO_NUGET_PACKAGES=$packages" >> $env:GITHUB_ENV
}
