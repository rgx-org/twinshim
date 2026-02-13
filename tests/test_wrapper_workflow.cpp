#include "common/arg_quote.h"
#include "common/local_registry_store.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace hklmwrap;

namespace {

struct ChildRunResult {
  DWORD exitCode = 0;
  std::string mergedOutput;
};

std::wstring GetModuleFilePath() {
  std::wstring path;
  path.resize(32768);
  DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (count == 0) {
    return {};
  }
  path.resize(count);
  return path;
}

std::optional<ChildRunResult> RunWithCapturedOutput(const std::wstring& exePath,
                                                    const std::vector<std::wstring>& args,
                                                    const std::wstring& workingDir) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
    return std::nullopt;
  }

  if (!SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(readPipe);
    CloseHandle(writePipe);
    return std::nullopt;
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;

  PROCESS_INFORMATION pi{};
  std::wstring cmd = BuildCommandLine(exePath, args);

  const BOOL ok = CreateProcessW(exePath.c_str(),
                                 cmd.data(),
                                 nullptr,
                                 nullptr,
                                 TRUE,
                                 CREATE_UNICODE_ENVIRONMENT,
                                 nullptr,
                                 workingDir.empty() ? nullptr : workingDir.c_str(),
                                 &si,
                                 &pi);

  CloseHandle(writePipe);
  writePipe = nullptr;

  if (!ok) {
    CloseHandle(readPipe);
    return std::nullopt;
  }

  std::string captured;
  char buffer[2048];
  DWORD bytesRead = 0;
  while (ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &bytesRead, nullptr) && bytesRead > 0) {
    captured.append(buffer, buffer + bytesRead);
  }

  CloseHandle(readPipe);
  readPipe = nullptr;

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return ChildRunResult{exitCode, captured};
}

std::wstring ReadRegSzText(const StoredValue& stored) {
  if (stored.data.empty()) {
    return {};
  }
  std::wstring text;
  const size_t wcharCount = stored.data.size() / sizeof(wchar_t);
  text.resize(wcharCount);
  std::memcpy(text.data(), stored.data.data(), wcharCount * sizeof(wchar_t));
  const size_t nul = text.find(L'\0');
  if (nul != std::wstring::npos) {
    text.resize(nul);
  }
  return text;
}

} // namespace

TEST_CASE("wrapper debug mode covers injected hook and store data flow", "[shim][workflow]") {
  const std::filesystem::path testExePath = GetModuleFilePath();
  REQUIRE_FALSE(testExePath.empty());

  const std::filesystem::path testsDir = testExePath.parent_path();
  const std::filesystem::path buildDir = testsDir.parent_path();

  const std::filesystem::path wrapperPath = buildDir / "hklm_wrapper_cli.exe";
  const std::filesystem::path probePath = testsDir / "hklm_workflow_probe.exe";
  const std::filesystem::path dbPath = buildDir / "hklm_workflow_probe-HKLM.sqlite";

  REQUIRE(std::filesystem::exists(wrapperPath));
  REQUIRE(std::filesystem::exists(probePath));

  std::error_code removeEc;
  std::filesystem::remove(dbPath, removeEc);

  const std::wstring suffix =
      L"e2e-" + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L"-" +
      std::to_wstring(static_cast<unsigned long>(GetTickCount64()));

  const auto run = RunWithCapturedOutput(wrapperPath.wstring(),
                                         {L"--debug", L"all", probePath.wstring(), suffix},
                                         buildDir.wstring());

  REQUIRE(run.has_value());
  REQUIRE(run->exitCode == 0);

  const std::string expectedKey = "HKLM\\Software\\hklm-wrapper-workflow\\e2e-";
  CHECK(run->mergedOutput.find("api=RegCreateKeyExW op=create_key") != std::string::npos);
  CHECK(run->mergedOutput.find(expectedKey) != std::string::npos);
  CHECK(run->mergedOutput.find("api=RegSetValueExW op=set_value") != std::string::npos);
  CHECK(run->mergedOutput.find("name=\"WorkflowValue\"") != std::string::npos);
  CHECK(run->mergedOutput.find("wrapped-ok") != std::string::npos);
  CHECK(run->mergedOutput.find("api=RegQueryValueExW op=query_value") != std::string::npos);
  CHECK(run->mergedOutput.find("rc=0 type=REG_SZ") != std::string::npos);

  LocalRegistryStore store;
  REQUIRE(store.Open(dbPath.wstring()));

  const std::wstring keyPath = L"HKLM\\Software\\hklm-wrapper-workflow\\" + suffix;
  const auto stored = store.GetValue(keyPath, L"WorkflowValue");
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);
  CHECK(stored->type == REG_SZ);
  CHECK(ReadRegSzText(*stored) == L"wrapped-ok");
}
