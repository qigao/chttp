@echo off
setlocal
if "%VSDEVCMD_BAT%"=="" (
  echo VSDEVCMD_BAT is required 1>&2
  exit /b 2
)
if not exist "%VSDEVCMD_BAT%" (
  echo VSDEVCMD_BAT does not exist: "%VSDEVCMD_BAT%" 1>&2
  exit /b 2
)
rem VsDevCmd may select Visual Studio's bundled vcpkg; retain the configured one.
set "CHTTP_CONFIGURED_VCPKG_ROOT=%VCPKG_ROOT%"
call "%VSDEVCMD_BAT%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b %errorlevel%
set "VCPKG_ROOT=%CHTTP_CONFIGURED_VCPKG_ROOT%"
rem Use the same VS instance for vcpkg and the project's compiler.
for %%I in ("%VSINSTALLDIR%.") do set "VCPKG_VISUAL_STUDIO_PATH=%%~fI"
%*
exit /b %errorlevel%
