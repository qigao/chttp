param([Parameter(Mandatory=$true)][string]$Rid, [switch]$Local)
$ErrorActionPreference = "Stop"
# Keep the PowerShell entry point for existing callers; Bash owns SDK restore.
if (-not $env:BASH_EXECUTABLE -or -not (Test-Path -LiteralPath $env:BASH_EXECUTABLE -PathType Leaf)) {
  throw "BASH_EXECUTABLE must name the Git for Windows bash.exe"
}
$previousRoot = $env:SDK_RESTORE_ROOT
try {
  if ($Local -and -not $env:SDK_RESTORE_ROOT) {
    $env:SDK_RESTORE_ROOT = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../build/native-sdk"))
  }
  $format = if ($Local) { "json" } else { "github" }
  $script = (Join-Path $PSScriptRoot "restore-native-sdks.sh").Replace('\', '/')
  $output = & $env:BASH_EXECUTABLE $script $Rid false $format
  if ($LASTEXITCODE -ne 0) { throw "failed to restore native SDKs (exit $LASTEXITCODE)" }
  if ($Local) {
    $roots = ($output -join "`n") | ConvertFrom-Json -AsHashtable
    foreach ($name in @('SALTS_ROOT', 'SALTS_UTILS_ROOT', 'QIGAO_NUGET_PACKAGES')) {
      [Environment]::SetEnvironmentVariable($name, $roots[$name], 'Process')
    }
  }
} finally {
  $env:SDK_RESTORE_ROOT = $previousRoot
}
