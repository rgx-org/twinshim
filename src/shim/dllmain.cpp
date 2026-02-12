#include "shim/registry_hooks.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;
  if (fdwReason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hinstDLL);
    hklmwrap::InstallRegistryHooks();
  } else if (fdwReason == DLL_PROCESS_DETACH) {
    hklmwrap::RemoveRegistryHooks();
  }
  return TRUE;
}
