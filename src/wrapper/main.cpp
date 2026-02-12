#include "common/arg_quote.h"
#include "common/path_util.h"
#include "common/win32_error.h"

#include "wrapper/process_inject.h"

#include "wrapper_config.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <cwctype>

using namespace hklmwrap;

static std::vector<std::wstring> GetRawArgs() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> out;
  if (!argv) {
    return out;
  }
  for (int i = 1; i < argc; i++) {
    out.emplace_back(argv[i]);
  }
  LocalFree(argv);
  return out;
}

static std::wstring BuildUsageMessage() {
  return L"Usage:\n"
         L"  hklm_wrapper.exe <target_exe> [target arguments...]\n\n"
         L"Examples:\n"
         L"  hklm_wrapper.exe C:\\Apps\\TargetApp.exe\n"
         L"  hklm_wrapper.exe C:\\Apps\\TargetApp.exe --mode test --config \"C:\\path with spaces\\cfg.json\"";
}

static int ParseLaunchArguments(std::wstring& targetExe, std::vector<std::wstring>& forwardedArgs) {
  const std::vector<std::wstring> rawArgs = GetRawArgs();
  if (rawArgs.empty()) {
    MessageBoxW(nullptr, BuildUsageMessage().c_str(), L"hklm_wrapper", MB_ICONERROR);
    return 1;
  }

  if (rawArgs[0] == L"-h" || rawArgs[0] == L"--help" || rawArgs[0] == L"/?") {
    MessageBoxW(nullptr, BuildUsageMessage().c_str(), L"hklm_wrapper", MB_ICONINFORMATION);
    return 0;
  }

  targetExe = rawArgs[0];
  forwardedArgs.assign(rawArgs.begin() + 1, rawArgs.end());
  return -1;
}

static std::wstring DefaultWorkingDirForTarget(const std::wstring& targetExe) {
  if (std::wstring(HKLM_WRAPPER_WORKING_DIR).empty()) {
    return GetDirectoryName(targetExe);
  }
  return std::wstring(HKLM_WRAPPER_WORKING_DIR);
}

static std::wstring ToLowerCopy(const std::wstring& value) {
  std::wstring lower = value;
  for (wchar_t& ch : lower) {
    ch = std::towlower(ch);
  }
  return lower;
}

static bool ContainsRunAsInvokerToken(const std::wstring& compatLayer) {
  const std::wstring lower = ToLowerCopy(compatLayer);
  size_t pos = 0;
  while (pos < lower.size()) {
    while (pos < lower.size() && lower[pos] == L' ') {
      pos++;
    }
    if (pos >= lower.size()) {
      break;
    }
    size_t end = pos;
    while (end < lower.size() && lower[end] != L' ') {
      end++;
    }
    if (lower.substr(pos, end - pos) == L"runasinvoker") {
      return true;
    }
    pos = end;
  }
  return false;
}

struct CompatLayerGuard {
  bool hadOriginal = false;
  std::wstring original;
  bool active = false;

  bool EnableRunAsInvoker() {
    DWORD required = GetEnvironmentVariableW(L"__COMPAT_LAYER", nullptr, 0);
    if (required > 0) {
      hadOriginal = true;
      original.resize(required - 1);
      GetEnvironmentVariableW(L"__COMPAT_LAYER", original.data(), required);
      if (ContainsRunAsInvokerToken(original)) {
        active = true;
        return true;
      }
      const std::wstring merged = original + L" RunAsInvoker";
      if (!SetEnvironmentVariableW(L"__COMPAT_LAYER", merged.c_str())) {
        return false;
      }
      active = true;
      return true;
    }

    if (!SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker")) {
      return false;
    }
    active = true;
    return true;
  }

  void Restore() {
    if (!active) {
      return;
    }
    if (hadOriginal) {
      SetEnvironmentVariableW(L"__COMPAT_LAYER", original.c_str());
      return;
    }
    SetEnvironmentVariableW(L"__COMPAT_LAYER", nullptr);
  }

  ~CompatLayerGuard() {
    Restore();
  }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::wstring targetExe;
  std::vector<std::wstring> args;
  int parseResult = ParseLaunchArguments(targetExe, args);
  if (parseResult >= 0) {
    return parseResult;
  }

  const std::wstring wrapperDir = GetDirectoryName(GetModulePath());
  const std::wstring targetStem = GetFileStem(targetExe);
  const std::wstring dbPath = CombinePath(wrapperDir, targetStem + L"-HKLM.sqlite");

  const std::wstring shimPath = CombinePath(wrapperDir, HKLM_WRAPPER_SHIM_DLL_NAME);
  SetEnvironmentVariableW(L"HKLM_WRAPPER_DB_PATH", dbPath.c_str());

  std::wstring cmdLine = BuildCommandLine(targetExe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::wstring mutableCmd = cmdLine;
  std::wstring workDir = DefaultWorkingDirForTarget(targetExe);

#if HKLM_WRAPPER_IGNORE_EMBEDDED_MANIFEST
  CompatLayerGuard compatLayerGuard;
  if (!compatLayerGuard.EnableRunAsInvoker()) {
    std::wstring msg = L"Failed to set __COMPAT_LAYER=RunAsInvoker: " + FormatWin32Error(GetLastError());
    MessageBoxW(nullptr, msg.c_str(), L"hklm_wrapper", MB_ICONERROR);
    return 3;
  }
#endif

  DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  BOOL ok = CreateProcessW(
      targetExe.c_str(),
      mutableCmd.data(),
      nullptr,
      nullptr,
      FALSE,
      flags,
      nullptr,
      workDir.empty() ? nullptr : workDir.c_str(),
      &si,
      &pi);

  if (!ok) {
    auto err = GetLastError();
    std::wstring msg = L"CreateProcessW failed: " + FormatWin32Error(err);
    MessageBoxW(nullptr, msg.c_str(), L"hklm_wrapper", MB_ICONERROR);
    return (int)err;
  }

  if (!InjectDllIntoProcess(pi.hProcess, shimPath)) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MessageBoxW(nullptr, L"Failed to inject hklm_shim.dll into target process", L"hklm_wrapper", MB_ICONERROR);
    return 2;
  }

  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  return (int)exitCode;
}
