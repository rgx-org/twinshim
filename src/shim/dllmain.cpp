#include "shim/registry_hooks.h"

#include <windows.h>

#include <string>

namespace {

void ShimTrace(const char* text) {
  if (!text || !*text) {
    return;
  }

  wchar_t pipeBuf[512]{};
  DWORD pipeLen = GetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", pipeBuf, (DWORD)(sizeof(pipeBuf) / sizeof(pipeBuf[0])));
  if (!pipeLen || pipeLen >= (sizeof(pipeBuf) / sizeof(pipeBuf[0]))) {
    return;
  }

  HANDLE h = CreateFileW(std::wstring(pipeBuf, pipeBuf + pipeLen).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD written = 0;
  WriteFile(h, text, (DWORD)lstrlenA(text), &written, nullptr);
  CloseHandle(h);
}

volatile LONG g_hooksInstalled = 0;

DWORD WINAPI HookInitThreadProc(LPVOID) {
  ShimTrace("[shim] hook init thread started\n");
  const bool installed = hklmwrap::InstallRegistryHooks();
  InterlockedExchange(&g_hooksInstalled, installed ? 1 : -1);
  ShimTrace(installed ? "[shim] hook install succeeded\n" : "[shim] hook install failed\n");
  return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  (void)hinstDLL;
  (void)lpvReserved;
  if (fdwReason == DLL_PROCESS_ATTACH) {
    ShimTrace("[shim] DLL_PROCESS_ATTACH\n");
    DisableThreadLibraryCalls(hinstDLL);
    HANDLE initThread = CreateThread(nullptr, 0, &HookInitThreadProc, nullptr, 0, nullptr);
    if (initThread) {
      CloseHandle(initThread);
    } else {
      InterlockedExchange(&g_hooksInstalled, -1);
      ShimTrace("[shim] failed to create hook init thread\n");
    }
  } else if (fdwReason == DLL_PROCESS_DETACH) {
    ShimTrace("[shim] DLL_PROCESS_DETACH\n");
    if (InterlockedCompareExchange(&g_hooksInstalled, 0, 0) == 1) {
      hklmwrap::RemoveRegistryHooks();
      ShimTrace("[shim] hooks removed\n");
    }
  }
  return TRUE;
}
