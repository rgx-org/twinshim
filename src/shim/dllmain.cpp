#include "shim/registry_hooks.h"

#include "shim/d3d9_surface_scaler.h"
#include "shim/ddraw_surface_scaler.h"

#include <windows.h>

namespace {

static DWORD GetEnvVarCompat(const wchar_t* primary, const wchar_t* legacy, wchar_t* buf, DWORD bufCount) {
  if (!buf || bufCount == 0) {
    return 0;
  }
  if (primary && *primary) {
    DWORD n = GetEnvironmentVariableW(primary, buf, bufCount);
    if (n && n < bufCount) {
      return n;
    }
  }
  if (legacy && *legacy) {
    DWORD n = GetEnvironmentVariableW(legacy, buf, bufCount);
    if (n && n < bufCount) {
      return n;
    }
  }
  return 0;
}

void SignalHookReadyEvent() {
  wchar_t nameBuf[512]{};
  DWORD nameLen = GetEnvVarCompat(
      L"TWINSHIM_HOOK_READY_EVENT",
      L"HKLM_WRAPPER_HOOK_READY_EVENT",
      nameBuf,
      (DWORD)(sizeof(nameBuf) / sizeof(nameBuf[0])));
  if (!nameLen || nameLen >= (sizeof(nameBuf) / sizeof(nameBuf[0]))) {
    return;
  }
  nameBuf[nameLen] = L'\0';

  HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, nameBuf);
  if (!ev) {
    return;
  }
  SetEvent(ev);
  CloseHandle(ev);
}

void ShimTrace(const char* text) {
  if (!text || !*text) {
    return;
  }

  wchar_t pipeBuf[512]{};
  DWORD pipeLen = GetEnvVarCompat(
      L"TWINSHIM_DEBUG_PIPE",
      L"HKLM_WRAPPER_DEBUG_PIPE",
      pipeBuf,
      (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }
  pipeBuf[pipeLen] = L'\0';

  HANDLE h = CreateFileW(pipeBuf, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD written = 0;
  WriteFile(h, text, (DWORD)lstrlenA(text), &written, nullptr);
  CloseHandle(h);
}

volatile LONG g_hooksInstalled = 0;
HANDLE g_hookInitThread = nullptr;

DWORD WINAPI HookInitThreadProc(LPVOID) {
  ShimTrace("[shim] hook init thread started\n");

  const bool installed = hklmwrap::InstallRegistryHooks();

  // Install optional D3D9 Present scaling hooks (best-effort, async).
  (void)hklmwrap::InstallD3D9SurfaceScalerHooks();

  // Install optional DirectDraw scaling hooks (system ddraw.dll paths only).
  (void)hklmwrap::InstallDDrawSurfaceScalerHooks();

  if (!installed) {
    InterlockedExchange(&g_hooksInstalled, -1);
    ShimTrace("[shim] hook install failed\n");
    SignalHookReadyEvent();
  } else if (hklmwrap::AreRegistryHooksActive()) {
    InterlockedExchange(&g_hooksInstalled, 1);
    ShimTrace("[shim] hook install succeeded\n");
    SignalHookReadyEvent();
  } else {
    InterlockedExchange(&g_hooksInstalled, 0);
    ShimTrace("[shim] hooks disabled by mode\n");
    SignalHookReadyEvent();
  }
  return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  if (fdwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinstDLL);
    g_hookInitThread = CreateThread(nullptr, 0, &HookInitThreadProc, nullptr, 0, nullptr);
    if (!g_hookInitThread) {
      InterlockedExchange(&g_hooksInstalled, -1);
    }
  } else if (fdwReason == DLL_PROCESS_DETACH) {
    // During process termination, avoid loader-lock-sensitive teardown.
    if (lpvReserved != nullptr) {
      return TRUE;
    }

    // Best-effort cleanup for optional D3D9 hooks.
    hklmwrap::RemoveD3D9SurfaceScalerHooks();

    // Best-effort cleanup for optional DirectDraw hooks.
    hklmwrap::RemoveDDrawSurfaceScalerHooks();

    HANDLE initThread = g_hookInitThread;
    g_hookInitThread = nullptr;
    if (initThread) {
      WaitForSingleObject(initThread, 2000);
      CloseHandle(initThread);
    }

    if (InterlockedCompareExchange(&g_hooksInstalled, 0, 0) == 1 && hklmwrap::AreRegistryHooksActive()) {
      hklmwrap::RemoveRegistryHooks();
    }
  }
  return TRUE;
}
