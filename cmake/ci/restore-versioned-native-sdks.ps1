param([Parameter(Mandatory=$true)][string]$Rid)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) { throw "GITHUB_TOKEN is required" }
$saltsVersion = if ($env:SALTS_SDK_VERSION) { $env:SALTS_SDK_VERSION } else { "1.3.0" }
$utilsVersion = if ($env:SALTS_UTILS_SDK_VERSION) { $env:SALTS_UTILS_SDK_VERSION } else { "3.1.0" }
$packages = if ($env:QIGAO_NUGET_PACKAGES) { $env:QIGAO_NUGET_PACKAGES } else { Join-Path $env:RUNNER_TEMP "qigao-nuget" }
$config = Join-Path $env:RUNNER_TEMP "qigao-nuget.config"
$project = Join-Path $env:RUNNER_TEMP "qigao-chttp-sdk-restore.csproj"
@'
<?xml version="1.0" encoding="utf-8"?>
<configuration><packageSources><clear /></packageSources></configuration>
'@ | Set-Content -LiteralPath $config
dotnet nuget add source https://nuget.pkg.github.com/qigao/index.json --name github --username qigao --password $env:GITHUB_TOKEN --store-password-in-clear-text --configfile $config
if ($LASTEXITCODE -ne 0) { throw "failed to configure GitHub Packages" }
@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[$saltsVersion]" />
    <PackageReference Include="SaltsUtils.Native" Version="[$utilsVersion]" />
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project
dotnet restore $project --packages $packages --configfile $config --no-cache
if ($LASTEXITCODE -ne 0) { throw "failed to restore native SDKs" }
$saltsRoot = Join-Path $packages "salts.native\$saltsVersion\sdk\$Rid"
$utilsRoot = Join-Path $packages "saltsutils.native\$utilsVersion\sdk\$Rid"
foreach ($p in @(
  (Join-Path $saltsRoot "lib\cmake\Salts\SaltsConfig.cmake"),
  (Join-Path $saltsRoot "include\cmeta\function.h"),
  (Join-Path $utilsRoot "lib\cmake\SaltsUtils\SaltsUtilsConfig.cmake"),
  (Join-Path $utilsRoot "include\data_bind_binding_plan.h")
)) {
  if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "missing restored SDK file: $p" }
}
"SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
"SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
"QIGAO_NUGET_PACKAGES=$packages" >> $env:GITHUB_ENV
