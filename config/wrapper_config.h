#pragma once

// Compile-time configuration for the wrapper.
//
// Notes:
// - The wrapper target EXE is provided at runtime as the first command-line argument.
// - All remaining command-line arguments are forwarded to the target EXE.
// - The injected DLL (hklm_shim.dll) will virtualize HKLM into a per-target SQLite DB.

// Optional: working directory to use for the target process. Leave empty to use target EXE directory.
#define HKLM_WRAPPER_WORKING_DIR L""

// The shim DLL file name (expected to live next to hklm_wrapper.exe unless you set HKLM_WRAPPER_SHIM_PATH).
#define HKLM_WRAPPER_SHIM_DLL_NAME L"hklm_shim.dll"

// Ignore requestedExecutionLevel in target embedded manifests by default (RunAsInvoker).
// Set to 0 to disable.
#define HKLM_WRAPPER_IGNORE_EMBEDDED_MANIFEST 1
