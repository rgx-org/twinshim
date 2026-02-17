@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "CMAKE_WRAPPER=%SCRIPT_DIR%cmake-msvc-x86.cmd"
if not exist "%CMAKE_WRAPPER%" (
  echo [ERROR] Missing helper script: "%CMAKE_WRAPPER%".
  exit /b 1
)

echo [INFO] Configuring preset windows-x86-msvc-release-stage ...

rem Auto-enable dgVoodoo AddOn build if the SDK is present in third_party.
set "DGVOODOO_SDK_DIR=%SCRIPT_DIR%..\third_party\dgvoodoo_addon_sdk"
set "DGVOODOO_HDR=%DGVOODOO_SDK_DIR%\Inc\Addon\AddonDefs.hpp"
set "DGVOODOO_LIB=%DGVOODOO_SDK_DIR%\Lib\x86\dgVoodooAddon.lib"

set "DGVOODOO_ADDON_FLAG="
if exist "%DGVOODOO_HDR%" if exist "%DGVOODOO_LIB%" (
  echo [INFO] dgVoodoo AddOn SDK detected; enabling SampleAddon.dll build
  set "DGVOODOO_ADDON_FLAG=-DHKLM_WRAPPER_ENABLE_DGVOODOO_ADDON=ON"
) else (
  echo [INFO] dgVoodoo AddOn SDK not detected; SampleAddon.dll will not be built
)

call "%CMAKE_WRAPPER%" --preset windows-x86-msvc-release-stage %DGVOODOO_ADDON_FLAG%
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Building preset windows-x86-msvc-release-stage ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage
if errorlevel 1 exit /b %errorlevel%

echo [INFO] Installing runtime binaries to stage/bin ...
call "%CMAKE_WRAPPER%" --build --preset windows-x86-msvc-release-stage-install
if errorlevel 1 exit /b %errorlevel%

echo [OK] Build complete. See stage\bin
