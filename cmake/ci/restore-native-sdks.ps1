param(
  [Parameter(Mandatory=$true)]
  [ValidateSet('windows-x64', 'linux-x64', 'linux-arm64', 'macos-arm64', 'android-arm64-v8a')]
  [string]$Rid,
  [string]$HostRid,
  [string]$Profile,
  [switch]$Local
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if ($Local) {
  $localFields = @(
    'VCPKG_CACHE_VERSION', 'SALTS_BASE_VERSION', 'CMETA_VERSION',
    'SALTS_UTILS_VERSION', 'SALTS_CORE_VERSION', 'CHTTP_VERSION',
    'VCPKG_ROOT', 'VCPKG_CACHE_REPOSITORY_ROOT', 'VCPKG_BINARY_SOURCES',
    'VSDEVCMD_BAT', 'CMAKE_BUILD_PARALLEL_LEVEL', 'MSVC_TARGET_TRIPLET',
    'LLVM_NATIVE_TARGET_TRIPLET', 'GCC_TARGET_TRIPLET', 'GCC_C_COMPILER',
    'GCC_CXX_COMPILER', 'LLVM_MINGW_ROOT', 'LLVM_MINGW_TARGET',
    'LLVM_MINGW_TOOLCHAIN_FILE', 'ANDROID_NDK_HOME'
  )
  foreach ($line in Get-Content -LiteralPath (Join-Path $repositoryRoot '.env')) {
    if ($line -match '^([^#=]+)=(.*)$' -and $localFields -contains $Matches[1]) {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
  }
} elseif (-not $env:GITHUB_ENV -or -not $env:GITHUB_PATH) {
  throw 'GITHUB_ENV and GITHUB_PATH are required in CI'
}

$arguments = @((Join-Path $PSScriptRoot 'restore-native-sdks.py'), '--rid', $Rid)
if ($HostRid) { $arguments += @('--host-rid', $HostRid) }
if ($Profile) { $arguments += @('--profile', $Profile) }
$restoredJson = & python @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$restored = ($restoredJson -join [Environment]::NewLine) | ConvertFrom-Json -AsHashtable
foreach ($key in $restored.Keys) {
  [Environment]::SetEnvironmentVariable($key, [string]$restored[$key], 'Process')
  if (-not $Local) {
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$key=$($restored[$key])"
  }
}
$runtimeBins = @(
  (Join-Path $restored['RE2C_ROOT'] 'bin')
  (Join-Path $restored['SALTS_BASE_ROOT'] 'bin')
  (Join-Path $restored['SALTS_UTILS_ROOT'] 'bin')
  (Join-Path $restored['SALTSCORE_ROOT'] 'bin')
  (Join-Path $restored['CHTTP_ROOT'] 'bin')
)
$env:PATH = ($runtimeBins -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH
if (-not $Local) {
  foreach ($runtimeBin in $runtimeBins) {
    Add-Content -LiteralPath $env:GITHUB_PATH -Value $runtimeBin
  }
}
Write-Host "Restored exact published SDKs for $Rid ($env:VCPKG_TARGET_TRIPLET)"
