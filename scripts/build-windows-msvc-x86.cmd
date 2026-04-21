@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "CMAKE_WRAPPER=%SCRIPT_DIR%cmake-msvc-x86.cmd"
if not exist "%CMAKE_WRAPPER%" (
  echo [ERROR] Missing helper script: "%CMAKE_WRAPPER%".
  exit /b 1
)

echo [INFO] Configuring preset windows-x86-msvc-release-stage ...

rem Informational: report whether the dgVoodoo AddOn SDK is present.
rem CMakeLists.txt auto-detects the SDK and enables the SampleAddon target,
rem so we no longer need to pass -D flags here (which broke on CMake < 3.21
rem when combined with --preset).
set "DGVOODOO_SDK_DIR=%SCRIPT_DIR%..\third_party\dgvoodoo_addon_sdk"
set "DGVOODOO_HDR=%DGVOODOO_SDK_DIR%\Inc\Addon\AddonDefs.hpp"
set "DGVOODOO_LIB=%DGVOODOO_SDK_DIR%\Lib\x86\dgVoodooAddon.lib"

if exist "%DGVOODOO_HDR%" (
  if exist "%DGVOODOO_LIB%" (
    echo [INFO] dgVoodoo AddOn SDK detected; SampleAddon.dll will be built
  ) else (
    echo [INFO] dgVoodoo AddOn SDK incomplete ^(missing lib^); SampleAddon.dll will not be built
  )
) else (
  echo [INFO] dgVoodoo AddOn SDK not found; SampleAddon.dll will not be built
)

call "%CMAKE_WRAPPER%" --preset windows-x86-msvc-release-stage
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Building preset windows-x86-msvc-release-stage ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Installing runtime binaries to stage/bin ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage-install
if errorlevel 1 exit /b %errorlevel%

echo [OK] Build complete. See stage\bin
