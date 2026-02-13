#include "shim/registry_hooks.h"

#include <windows.h>

namespace {

volatile LONG g_hooksInstalled = 0;

DWORD WINAPI HookInitThreadProc(LPVOID) {
  const bool installed = hklmwrap::InstallRegistryHooks();
  InterlockedExchange(&g_hooksInstalled, installed ? 1 : -1);
  return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;
  if (fdwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinstDLL);
    HANDLE initThread = CreateThread(nullptr, 0, &HookInitThreadProc, nullptr, 0, nullptr);
    if (initThread) {
      CloseHandle(initThread);
    } else {
      InterlockedExchange(&g_hooksInstalled, -1);
    }
  } else if (fdwReason == DLL_PROCESS_DETACH) {
    if (InterlockedCompareExchange(&g_hooksInstalled, 0, 0) == 1) {
      hklmwrap::RemoveRegistryHooks();
    }
  }
  return TRUE;
}
