#include "wrapper/process_inject.h"

#include <windows.h>

namespace hklmwrap {

bool InjectDllIntoProcess(HANDLE processHandle, const std::wstring& dllPath) {
  if (!processHandle || dllPath.empty()) {
    return false;
  }

  const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(processHandle, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remote) {
    return false;
  }

  SIZE_T written = 0;
  if (!WriteProcessMemory(processHandle, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (!kernel32) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }
  auto loadLibraryW = (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
  if (!loadLibraryW) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  HANDLE thread = CreateRemoteThread(processHandle, nullptr, 0, loadLibraryW, remote, 0, nullptr);
  if (!thread) {
    VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
    return false;
  }

  WaitForSingleObject(thread, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeThread(thread, &exitCode);
  CloseHandle(thread);
  VirtualFreeEx(processHandle, remote, 0, MEM_RELEASE);
  return exitCode != 0;
}

}
