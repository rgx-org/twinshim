#include "common/local_registry_store.h"
#include "common/utf8.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cstring>
#include <cwctype>

using namespace hklmwrap;

static void PrintUsage() {
  std::wcerr << L"hklmreg --db <path> <add|delete|export|import> [options]\n"
                L"\n"
                L"Commands (REG-like subset):\n"
                L"  add    <KeyName> /v <ValueName> [/t <Type>] /d <Data> [/f]\n"
                L"  delete <KeyName> [/v <ValueName>] [/f]\n"
                L"  export <FileName> [<KeyNamePrefix>]\n"
                L"  import <FileName>\n"
                L"\n"
                L"KeyName examples: HKLM\\Software\\MyApp or HKEY_LOCAL_MACHINE\\Software\\MyApp\n"
                L"Type: REG_SZ | REG_DWORD | REG_QWORD | REG_BINARY (default: REG_SZ)\n";
}

static std::wstring CanonKey(const std::wstring& in) {
  std::wstring s = in;
  for (auto& ch : s) {
    if (ch == L'/') ch = L'\\';
  }
  if (s.rfind(L"HKEY_LOCAL_MACHINE\\", 0) == 0) {
    s = L"HKLM\\" + s.substr(19);
  }
  if (s == L"HKEY_LOCAL_MACHINE") {
    s = L"HKLM";
  }
  return s;
}

static uint32_t ParseType(const std::wstring& t) {
  if (t == L"REG_DWORD") return REG_DWORD;
  if (t == L"REG_QWORD") return REG_QWORD;
  if (t == L"REG_BINARY") return REG_BINARY;
  return REG_SZ;
}

static std::vector<uint8_t> ParseData(uint32_t type, const std::wstring& dataText) {
  if (type == REG_DWORD) {
    uint32_t v = std::stoul(dataText, nullptr, 0);
    std::vector<uint8_t> out(4);
    std::memcpy(out.data(), &v, 4);
    return out;
  }
  if (type == REG_QWORD) {
    uint64_t v = std::stoull(dataText, nullptr, 0);
    std::vector<uint8_t> out(8);
    std::memcpy(out.data(), &v, 8);
    return out;
  }
  if (type == REG_BINARY) {
    // Accept hex pairs with optional separators (comma/space).
    std::vector<uint8_t> out;
    int hi = -1;
    for (wchar_t ch : dataText) {
      int v = -1;
      if (ch >= L'0' && ch <= L'9') v = ch - L'0';
      else if (ch >= L'a' && ch <= L'f') v = 10 + (ch - L'a');
      else if (ch >= L'A' && ch <= L'F') v = 10 + (ch - L'A');
      else continue;
      if (hi < 0) hi = v;
      else {
        out.push_back((uint8_t)((hi << 4) | v));
        hi = -1;
      }
    }
    return out;
  }
  // REG_SZ: UTF-16LE including null terminator.
  std::wstring s = dataText;
  std::vector<uint8_t> out((s.size() + 1) * sizeof(wchar_t));
  std::memcpy(out.data(), s.c_str(), out.size());
  return out;
}

static std::wstring EscapeRegString(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size() + 8);
  for (wchar_t ch : s) {
    if (ch == L'\\' || ch == L'\"') {
      out.push_back(L'\\');
    }
    out.push_back(ch);
  }
  return out;
}

static std::wstring UnescapeRegString(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  bool esc = false;
  for (wchar_t ch : s) {
    if (esc) {
      out.push_back(ch);
      esc = false;
      continue;
    }
    if (ch == L'\\') {
      esc = true;
      continue;
    }
    out.push_back(ch);
  }
  if (esc) {
    out.push_back(L'\\');
  }
  return out;
}

static std::wstring KeyToRegHeader(const std::wstring& keyPath) {
  if (keyPath == L"HKLM") {
    return L"[HKEY_LOCAL_MACHINE]";
  }
  if (keyPath.rfind(L"HKLM\\", 0) == 0) {
    return L"[HKEY_LOCAL_MACHINE\\" + keyPath.substr(5) + L"]";
  }
  return L"[" + keyPath + L"]";
}

static std::wstring ValueNameToReg(const std::wstring& name) {
  if (name.empty()) {
    return L"@";
  }
  return L"\"" + EscapeRegString(name) + L"\"";
}

static std::wstring BytesToHexCsv(const std::vector<uint8_t>& b) {
  static const wchar_t* hexd = L"0123456789abcdef";
  std::wstring out;
  for (size_t i = 0; i < b.size(); i++) {
    if (i) out.append(L",");
    out.push_back(hexd[(b[i] >> 4) & 0xF]);
    out.push_back(hexd[b[i] & 0xF]);
  }
  return out;
}

static std::wstring FormatRegLine(const std::wstring& valueName, uint32_t type, const std::vector<uint8_t>& data) {
  std::wstring left = ValueNameToReg(valueName);
  if (type == REG_DWORD && data.size() >= 4) {
    uint32_t v = 0;
    std::memcpy(&v, data.data(), 4);
    wchar_t buf[32];
    swprintf_s(buf, L"%08x", v);
    return left + L"=dword:" + buf;
  }
  if (type == REG_QWORD && data.size() >= 8) {
    // .reg represents QWORD as hex(b): with little-endian bytes.
    std::vector<uint8_t> b(data.begin(), data.begin() + 8);
    return left + L"=hex(b):" + BytesToHexCsv(b);
  }
  if (type == REG_SZ) {
    // data is UTF-16LE null-terminated.
    std::wstring s;
    if (!data.empty()) {
      s.assign((const wchar_t*)data.data(), data.size() / sizeof(wchar_t));
      auto nul = s.find(L'\0');
      if (nul != std::wstring::npos) {
        s.resize(nul);
      }
    }
    return left + L"=\"" + EscapeRegString(s) + L"\"";
  }
  // Default to binary.
  return left + L"=hex:" + BytesToHexCsv(data);
}

static bool WriteUtf16LeFile(const std::wstring& path, const std::wstring& content) {
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

static std::wstring Trim(const std::wstring& s) {
  size_t a = 0;
  while (a < s.size() && iswspace(s[a])) a++;
  size_t b = s.size();
  while (b > a && iswspace(s[b - 1])) b--;
  return s.substr(a, b - a);
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) {
    PrintUsage();
    return 2;
  }
  std::wstring dbPath;
  int i = 1;
  if (std::wstring(argv[i]) == L"--db" && i + 1 < argc) {
    dbPath = argv[i + 1];
    i += 2;
  } else {
    PrintUsage();
    return 2;
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
    std::wstring content = L"Windows Registry Editor Version 5.00\r\n\r\n";
    std::wstring currentKey;
    for (const auto& r : rows) {
      if (!prefix.empty()) {
        if (r.keyPath.rfind(prefix, 0) != 0) {
          continue;
        }
      }
      if (r.keyPath != currentKey) {
        currentKey = r.keyPath;
        content += KeyToRegHeader(currentKey);
        content += L"\r\n";
      }
      content += FormatRegLine(r.valueName, r.type, r.data);
      content += L"\r\n";
    }
    content += L"\r\n";
    if (!WriteUtf16LeFile(outPath, content)) {
      std::wcerr << L"Failed to write: " << outPath << L"\n";
      return 1;
    }
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

    std::wistringstream iss(text);
    std::wstring line;
    std::wstring currentKey;
    while (std::getline(iss, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == L';') {
        continue;
      }
      if (line.front() == L'[' && line.back() == L']') {
        bool del = false;
        std::wstring inside = line.substr(1, line.size() - 2);
        if (!inside.empty() && inside.front() == L'-') {
          del = true;
          inside.erase(inside.begin());
        }
        inside = CanonKey(inside);
        currentKey = inside;
        if (del) {
          store.DeleteKeyTree(currentKey);
        } else {
          store.PutKey(currentKey);
        }
        continue;
      }
      auto eq = line.find(L'=');
      if (eq == std::wstring::npos || currentKey.empty()) {
        continue;
      }
      std::wstring left = Trim(line.substr(0, eq));
      std::wstring right = Trim(line.substr(eq + 1));
      std::wstring valueName;
      if (left == L"@") {
        valueName.clear();
      } else if (left.size() >= 2 && left.front() == L'\"' && left.back() == L'\"') {
        valueName = UnescapeRegString(left.substr(1, left.size() - 2));
      } else {
        continue;
      }
      if (right == L"-") {
        store.DeleteValue(currentKey, valueName);
        continue;
      }
      if (right.size() >= 2 && right.front() == L'\"' && right.back() == L'\"') {
        std::wstring s = UnescapeRegString(right.substr(1, right.size() - 2));
        auto data = ParseData(REG_SZ, s);
        store.PutValue(currentKey, valueName, REG_SZ, data.data(), (uint32_t)data.size());
        continue;
      }
      if (right.rfind(L"dword:", 0) == 0) {
        std::wstring hex = right.substr(6);
        uint32_t v = std::stoul(hex, nullptr, 16);
        std::vector<uint8_t> data(4);
        std::memcpy(data.data(), &v, 4);
        store.PutValue(currentKey, valueName, REG_DWORD, data.data(), (uint32_t)data.size());
        continue;
      }
      if (right.rfind(L"hex:", 0) == 0) {
        std::wstring hex = right.substr(4);
        auto data = ParseData(REG_BINARY, hex);
        store.PutValue(currentKey, valueName, REG_BINARY, data.data(), (uint32_t)data.size());
        continue;
      }
      // hex(b) -> treat as QWORD binary
      if (right.rfind(L"hex(b):", 0) == 0) {
        std::wstring hex = right.substr(7);
        auto data = ParseData(REG_BINARY, hex);
        store.PutValue(currentKey, valueName, REG_QWORD, data.data(), (uint32_t)data.size());
        continue;
      }
    }
    return 0;
  }

  PrintUsage();
  return 2;
}
