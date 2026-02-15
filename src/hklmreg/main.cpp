#include "common/local_registry_store.h"
#include "common/utf8.h"
#include "hklmreg/reg_file.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cstring>
#include <cwctype>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

using namespace hklmwrap;

using hklmwrap::regfile::BuildRegExportContent;
using hklmwrap::regfile::CanonKey;
using hklmwrap::regfile::ParseData;
using hklmwrap::regfile::ParseType;

static void PrintUsage() {
  std::wcerr << L"hklmreg [--db <path>] <add|delete|export|import|dump> [options]\n"
                L"\n"
                L"Commands (REG-like subset):\n"
                L"  add    <KeyName> /v <ValueName> [/t <Type>] /d <Data> [/f]\n"
                L"  delete <KeyName> [/v <ValueName>] [/f]\n"
                L"  export <FileName> [<KeyNamePrefix>]\n"
                L"  dump   [<KeyNamePrefix>]\n"
                L"  import <FileName>\n"
                L"\n"
                L"Default DB: .\\HKLM.sqlite (current directory)\n"
                L"\n"
                L"KeyName examples: HKLM\\Software\\MyApp or HKEY_LOCAL_MACHINE\\Software\\MyApp\n"
                L"Type: REG_SZ | REG_DWORD | REG_QWORD | REG_BINARY (default: REG_SZ)\n";
}

static bool WriteUtf16LeFile(const std::wstring& path, const std::wstring& content) {
#if defined(_WIN32)
  static_assert(sizeof(wchar_t) == 2, "hklmreg expects UTF-16LE wchar_t");
#endif
  std::ofstream f(WideToUtf8(path), std::ios::binary);
  if (!f) return false;
  uint16_t bom = 0xFEFF;
  f.write((const char*)&bom, 2);
  f.write((const char*)content.data(), (std::streamsize)(content.size() * sizeof(wchar_t)));
  return true;
}

static std::wstring ReadWholeFileUtf16OrUtf8(const std::wstring& path) {
  std::ifstream f(WideToUtf8(path), std::ios::binary);
  if (!f) return {};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    // UTF-16LE
    size_t wcharCount = (bytes.size() - 2) / 2;
    std::wstring out;
    out.resize(wcharCount);
    std::memcpy(out.data(), bytes.data() + 2, wcharCount * 2);
    return out;
  }
  // Assume UTF-8
  std::string s(bytes.begin(), bytes.end());
  return Utf8ToWide(s);
}

#if defined(_WIN32)
static bool StdoutIsConsole() {
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
  DWORD mode = 0;
  return GetConsoleMode(h, &mode) != 0;
}
#endif

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }

  std::wstring dbPath = L"HKLM.sqlite";
  int i = 1;
  if (std::wstring(argv[i]) == L"--db") {
    if (i + 1 >= argc) {
      std::wcerr << L"Missing value for --db\n";
      PrintUsage();
      return 2;
    }
    dbPath = argv[i + 1];
    i += 2;
  }

  if (i >= argc) {
    PrintUsage();
    return 2;
  }
  std::wstring cmd = argv[i++];

  LocalRegistryStore store;
  if (!store.Open(dbPath)) {
    std::wcerr << L"Failed to open DB: " << dbPath << L"\n";
    return 1;
  }

  if (cmd == L"add") {
    if (i >= argc) {
      PrintUsage();
      return 2;
    }
    std::wstring key = CanonKey(argv[i++]);
    std::wstring valueName;
    std::wstring typeStr = L"REG_SZ";
    std::wstring dataStr;
    bool force = false;

    while (i < argc) {
      std::wstring opt = argv[i++];
      if (opt == L"/v" && i < argc) {
        valueName = argv[i++];
      } else if (opt == L"/ve") {
        valueName.clear();
      } else if (opt == L"/t" && i < argc) {
        typeStr = argv[i++];
      } else if (opt == L"/d" && i < argc) {
        dataStr = argv[i++];
      } else if (opt == L"/f") {
        force = true;
      } else {
        std::wcerr << L"Unknown option: " << opt << L"\n";
        return 2;
      }
    }
    (void)force;
    if (key.empty() || dataStr.empty()) {
      PrintUsage();
      return 2;
    }
    uint32_t type = ParseType(typeStr);
    auto data = ParseData(type, dataStr);
    if (!store.PutValue(key, valueName, type, data.data(), (uint32_t)data.size())) {
      std::wcerr << L"Failed to add value\n";
      return 1;
    }
    return 0;
  }

  if (cmd == L"delete") {
    if (i >= argc) {
      PrintUsage();
      return 2;
    }
    std::wstring key = CanonKey(argv[i++]);
    std::wstring valueName;
    bool hasValue = false;

    while (i < argc) {
      std::wstring opt = argv[i++];
      if (opt == L"/v" && i < argc) {
        valueName = argv[i++];
        hasValue = true;
      } else if (opt == L"/f") {
        // ignored
      } else {
        std::wcerr << L"Unknown option: " << opt << L"\n";
        return 2;
      }
    }

    if (hasValue) {
      if (!store.DeleteValue(key, valueName)) {
        std::wcerr << L"Failed to delete value\n";
        return 1;
      }
    } else {
      if (!store.DeleteKeyTree(key)) {
        std::wcerr << L"Failed to delete key\n";
        return 1;
      }
    }
    return 0;
  }

  if (cmd == L"export") {
    if (i >= argc) {
      PrintUsage();
      return 2;
    }
    std::wstring outPath = argv[i++];
    std::wstring prefix = (i < argc) ? CanonKey(argv[i++]) : L"";

    auto rows = store.ExportAll();
    std::wstring content = BuildRegExportContent(rows, prefix);
    if (!WriteUtf16LeFile(outPath, content)) {
      std::wcerr << L"Failed to write: " << outPath << L"\n";
      return 1;
    }
    return 0;
  }

  if (cmd == L"dump") {
    std::wstring prefix = (i < argc) ? CanonKey(argv[i++]) : L"";
    auto rows = store.ExportAll();
    std::wstring content = BuildRegExportContent(rows, prefix);

#if defined(_WIN32)
    if (StdoutIsConsole()) {
      HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
      DWORD written = 0;
      if (!WriteConsoleW(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr)) {
        std::wcerr << L"Failed to write to console\n";
        return 1;
      }
    } else {
      // When redirected/piped, write UTF-16LE with BOM so consumers can detect encoding.
      (void)_setmode(_fileno(stdout), _O_BINARY);
      const uint16_t bom = 0xFEFF;
      std::cout.write(reinterpret_cast<const char*>(&bom), 2);
      std::cout.write(reinterpret_cast<const char*>(content.data()),
                      static_cast<std::streamsize>(content.size() * sizeof(wchar_t)));
      std::cout.flush();
      if (!std::cout) {
        std::wcerr << L"Failed to write to stdout\n";
        return 1;
      }
    }
#else
    // Non-Windows builds (if any) emit UTF-8 to stdout.
    std::cout << WideToUtf8(content);
    if (!std::cout) {
      std::wcerr << L"Failed to write to stdout\n";
      return 1;
    }
#endif
    return 0;
  }

  if (cmd == L"import") {
    if (i >= argc) {
      PrintUsage();
      return 2;
    }
    std::wstring inPath = argv[i++];
    std::wstring text = ReadWholeFileUtf16OrUtf8(inPath);
    if (text.empty()) {
      std::wcerr << L"Failed to read: " << inPath << L"\n";
      return 1;
    }

    if (!hklmwrap::regfile::ImportRegText(store, text)) {
      std::wcerr << L"Import failed\n";
      return 1;
    }
    return 0;
  }

  PrintUsage();
  return 2;
}
