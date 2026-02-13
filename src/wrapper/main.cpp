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
#include <cstdio>
#include <thread>
#include <sstream>

using namespace hklmwrap;

static void ShowError(const std::wstring& message) {
#if defined(HKLM_WRAPPER_CONSOLE_APP)
  fwprintf(stderr, L"%ls\n", message.c_str());
  fflush(stderr);
#else
  MessageBoxW(nullptr, message.c_str(), L"hklm_wrapper", MB_ICONERROR);
#endif
}

static void ShowInfo(const std::wstring& message) {
#if defined(HKLM_WRAPPER_CONSOLE_APP)
  fwprintf(stdout, L"%ls\n", message.c_str());
  fflush(stdout);
#else
  MessageBoxW(nullptr, message.c_str(), L"hklm_wrapper", MB_ICONINFORMATION);
#endif
}

static void TraceLine(const std::wstring& message, bool enabled) {
#if defined(HKLM_WRAPPER_CONSOLE_APP)
  if (!enabled) {
    return;
  }
  fwprintf(stdout, L"[hklm_wrapper] %ls\n", message.c_str());
  fflush(stdout);
#else
  (void)message;
  (void)enabled;
#endif
}

static bool TryQueryWow64(HANDLE process, BOOL* isWow64) {
  if (!isWow64) {
    return false;
  }
  auto isWow64ProcessFn = reinterpret_cast<BOOL(WINAPI*)(HANDLE, PBOOL)>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process"));
  if (!isWow64ProcessFn) {
    return false;
  }
  return isWow64ProcessFn(process, isWow64) != FALSE;
}

static bool IsProcessBitnessMismatched(HANDLE targetProcess) {
  BOOL selfWow64 = FALSE;
  BOOL targetWow64 = FALSE;
  if (!TryQueryWow64(GetCurrentProcess(), &selfWow64)) {
    return false;
  }
  if (!TryQueryWow64(targetProcess, &targetWow64)) {
    return false;
  }
  return selfWow64 != targetWow64;
}

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

static const wchar_t* GetWrapperExeNameForUsage() {
#if defined(HKLM_WRAPPER_CONSOLE_APP)
  return L"hklm_wrapper_cli.exe";
#else
  return L"hklm_wrapper.exe";
#endif
}

static std::wstring BuildUsageMessage() {
  const std::wstring exe = GetWrapperExeNameForUsage();
  return L"Usage:\n"
         L"  " + exe + L" [--debug <api1,api2,...|all>] <target_exe> [target arguments...]\n\n"
         L"Examples:\n"
         L"  " + exe + L" C:\\Apps\\TargetApp.exe\n"
         L"  " + exe + L" --debug RegOpenKey,RegQueryValue C:\\Apps\\TargetApp.exe\n"
         L"  " + exe + L" C:\\Apps\\TargetApp.exe --mode test --config \"C:\\path with spaces\\cfg.json\"";
}

static int ParseLaunchArguments(std::wstring& targetExe,
                                std::vector<std::wstring>& forwardedArgs,
                                std::wstring& debugApisCsv) {
  const std::vector<std::wstring> rawArgs = GetRawArgs();
  if (rawArgs.empty()) {
    ShowError(BuildUsageMessage());
    return 1;
  }

  if (rawArgs[0] == L"-h" || rawArgs[0] == L"--help" || rawArgs[0] == L"/?") {
    ShowInfo(BuildUsageMessage());
    return 0;
  }

  size_t i = 0;
  while (i < rawArgs.size()) {
    if (rawArgs[i] == L"--debug") {
      if (i + 1 >= rawArgs.size()) {
        ShowError(L"Missing value for --debug. Expected comma-separated API list or all.");
        return 1;
      }
      debugApisCsv = rawArgs[i + 1];
      i += 2;
      continue;
    }
    break;
  }

  if (i >= rawArgs.size()) {
    ShowError(BuildUsageMessage());
    return 1;
  }

  targetExe = rawArgs[i];
  forwardedArgs.assign(rawArgs.begin() + i + 1, rawArgs.end());
  return -1;
}

static bool EnsureStdoutBoundToConsole() {
  auto hasValidStdHandle = [](DWORD stdId) {
    HANDLE handle = GetStdHandle(stdId);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    SetLastError(ERROR_SUCCESS);
    const DWORD type = GetFileType(handle);
    if (type == FILE_TYPE_UNKNOWN && GetLastError() != ERROR_SUCCESS) {
      return false;
    }
    return true;
  };

  if (hasValidStdHandle(STD_OUTPUT_HANDLE) && hasValidStdHandle(STD_ERROR_HANDLE)) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
  }

  if (!AttachConsole(ATTACH_PARENT_PROCESS) && GetLastError() != ERROR_ACCESS_DENIED) {
    if (!AllocConsole()) {
      return false;
    }
  }

  FILE* out = nullptr;
  FILE* err = nullptr;
  if (freopen_s(&out, "CONOUT$", "w", stdout) != 0) {
    return false;
  }
  if (freopen_s(&err, "CONOUT$", "w", stderr) != 0) {
    return false;
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  return true;
}

static HANDLE CreateProcessTrackingJob() {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job) {
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    return nullptr;
  }

  return job;
}

static bool WaitForJobToDrain(HANDLE job) {
  if (!job) {
    return false;
  }

  while (true) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) {
      return false;
    }
    if (info.ActiveProcesses == 0) {
      return true;
    }
    Sleep(50);
  }
}

struct DebugPipeBridge {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  std::thread reader;
  std::wstring pipeName;

  bool Start() {
    wchar_t pipePath[256]{};
    swprintf_s(pipePath, L"\\\\.\\pipe\\hklm_wrapper_debug_%lu", GetCurrentProcessId());
    pipeName = pipePath;

    pipe = CreateNamedPipeW(pipeName.c_str(),
                            PIPE_ACCESS_INBOUND,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                            1,
                            4096,
                            4096,
                            0,
                            nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      return false;
    }

    reader = std::thread([this] {
      BOOL connected = ConnectNamedPipe(pipe, nullptr);
      if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        return;
      }

      char buffer[1024];
      while (true) {
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer, (DWORD)sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0) {
          break;
        }
        fwrite(buffer, 1, bytesRead, stdout);
        fflush(stdout);
      }
    });
    return true;
  }

  void Stop() {
    if (pipe != INVALID_HANDLE_VALUE && !pipeName.empty()) {
      HANDLE unblockClient = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (unblockClient != INVALID_HANDLE_VALUE) {
        CloseHandle(unblockClient);
      }
    }
    if (reader.joinable()) {
      reader.join();
    }
    if (pipe != INVALID_HANDLE_VALUE) {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
    }
  }

  ~DebugPipeBridge() {
    Stop();
  }
};

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

#if defined(HKLM_WRAPPER_CONSOLE_APP)
int wmain() {
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#endif
  std::wstring targetExe;
  std::vector<std::wstring> args;
  std::wstring debugApisCsv;
  int parseResult = ParseLaunchArguments(targetExe, args, debugApisCsv);
  if (parseResult >= 0) {
    return parseResult;
  }

  const bool traceEnabled = !debugApisCsv.empty();

  const std::wstring wrapperDir = GetDirectoryName(GetModulePath());
  const std::wstring targetStem = GetFileStem(targetExe);
  const std::wstring dbPath = CombinePath(wrapperDir, targetStem + L"-HKLM.sqlite");

  const std::wstring shimPath = CombinePath(wrapperDir, HKLM_WRAPPER_SHIM_DLL_NAME);
  SetEnvironmentVariableW(L"HKLM_WRAPPER_DB_PATH", dbPath.c_str());

  DebugPipeBridge debugBridge;
  if (!debugApisCsv.empty()) {
    if (!EnsureStdoutBoundToConsole()) {
      ShowError(L"Failed to bind stdout to console for --debug mode.");
      return 4;
    }
    TraceLine(L"debug mode enabled", traceEnabled);
    if (!debugBridge.Start()) {
      std::wstring msg = L"Failed to create debug pipe: " + FormatWin32Error(GetLastError());
      ShowError(msg);
      return 5;
    }
    TraceLine(L"debug pipe created: " + debugBridge.pipeName, traceEnabled);
    SetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_APIS", debugApisCsv.c_str());
    SetEnvironmentVariableW(L"HKLM_WRAPPER_DEBUG_PIPE", debugBridge.pipeName.c_str());
  }

  std::wstring cmdLine = BuildCommandLine(targetExe, args);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  std::wstring mutableCmd = cmdLine;
  std::wstring workDir = DefaultWorkingDirForTarget(targetExe);

  TraceLine(L"launching target: " + targetExe, traceEnabled);
  if (!workDir.empty()) {
    TraceLine(L"working directory: " + workDir, traceEnabled);
  }

#if HKLM_WRAPPER_IGNORE_EMBEDDED_MANIFEST
  CompatLayerGuard compatLayerGuard;
  if (!compatLayerGuard.EnableRunAsInvoker()) {
    std::wstring msg = L"Failed to set __COMPAT_LAYER=RunAsInvoker: " + FormatWin32Error(GetLastError());
    ShowError(msg);
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
    ShowError(msg);
    return (int)err;
  }

  TraceLine(L"CreateProcessW succeeded", traceEnabled);

  if (IsProcessBitnessMismatched(pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ShowError(L"Wrapper/target architecture mismatch detected. Ensure hklm_wrapper_cli.exe, hklm_shim.dll, and target EXE have the same bitness (all x86 or all x64).");
    return 6;
  }

  TraceLine(L"injecting shim: " + shimPath, traceEnabled);

  if (!InjectDllIntoProcess(pi.hProcess, shimPath)) {
    DWORD injectErr = GetLastError();
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    std::wstring msg = L"Failed to inject hklm_shim.dll into target process: " + FormatWin32Error(injectErr);
    ShowError(msg);
    return 2;
  }

  TraceLine(L"shim injected successfully", traceEnabled);

  HANDLE debugJob = nullptr;
  bool trackWithDebugJob = false;
  bool waitedForJob = false;
  if (!debugApisCsv.empty()) {
    debugJob = CreateProcessTrackingJob();
    if (debugJob && AssignProcessToJobObject(debugJob, pi.hProcess)) {
      trackWithDebugJob = true;
    }
  }

  ResumeThread(pi.hThread);
  TraceLine(L"target resumed", traceEnabled);
  CloseHandle(pi.hThread);

  if (trackWithDebugJob) {
    TraceLine(L"waiting for job-tracked process tree to exit", traceEnabled);
    if (debugJob) {
      waitedForJob = WaitForJobToDrain(debugJob);
    }
  }
  if (debugJob) {
    CloseHandle(debugJob);
    debugJob = nullptr;
  }

  if (!waitedForJob) {
    TraceLine(L"waiting for target process handle to signal", traceEnabled);
    WaitForSingleObject(pi.hProcess, INFINITE);
  }
  TraceLine(L"wait complete; stopping debug pipe bridge", traceEnabled);
  debugBridge.Stop();
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  std::wstringstream exitMsg;
  exitMsg << L"wrapper returning exit code " << static_cast<unsigned long>(exitCode)
          << L" (0x" << std::hex << std::uppercase << static_cast<unsigned long>(exitCode) << L")";
  TraceLine(exitMsg.str(), traceEnabled);
  CloseHandle(pi.hProcess);
  return (int)exitCode;
}
