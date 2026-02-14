#include "hklmreg/reg_file.h"

#include "common/utf8.h"

#include <cstring>
#include <cwctype>
#include <map>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
// Keep these aligned with Win32 registry type IDs.
#ifndef REG_SZ
constexpr uint32_t REG_SZ = 1;
#endif
#ifndef REG_BINARY
constexpr uint32_t REG_BINARY = 3;
#endif
#ifndef REG_DWORD
constexpr uint32_t REG_DWORD = 4;
#endif
#ifndef REG_QWORD
constexpr uint32_t REG_QWORD = 11;
#endif
#endif

namespace hklmwrap::regfile {

static std::wstring Trim(const std::wstring& s) {
  size_t a = 0;
  while (a < s.size() && iswspace(s[a])) a++;
  size_t b = s.size();
  while (b > a && iswspace(s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::wstring CanonKey(const std::wstring& in) {
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

uint32_t ParseType(const std::wstring& t) {
  if (t == L"REG_DWORD") return REG_DWORD;
  if (t == L"REG_QWORD") return REG_QWORD;
  if (t == L"REG_BINARY") return REG_BINARY;
  return REG_SZ;
}

std::vector<uint8_t> ParseData(uint32_t type, const std::wstring& dataText) {
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
  // REG_SZ: UTF-16LE including null terminator on Windows; for tests / non-Windows
  // builds this is a wchar_t-native encoding that round-trips within the process.
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
#if defined(_WIN32)
    swprintf_s(buf, L"%08x", v);
#else
    swprintf(buf, 32, L"%08x", v);
#endif
    return left + L"=dword:" + buf;
  }
  if (type == REG_QWORD && data.size() >= 8) {
    // .reg represents QWORD as hex(b): with little-endian bytes.
    std::vector<uint8_t> b(data.begin(), data.begin() + 8);
    return left + L"=hex(b):" + BytesToHexCsv(b);
  }
  if (type == REG_SZ) {
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
  return left + L"=hex:" + BytesToHexCsv(data);
}

std::wstring BuildRegExportContent(const std::vector<LocalRegistryStore::ExportRow>& rows, const std::wstring& prefix) {
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
    if (!r.isKeyOnly) {
      content += FormatRegLine(r.valueName, r.type, r.data);
      content += L"\r\n";
    }
  }
  content += L"\r\n";
  return content;
}

bool ImportRegText(LocalRegistryStore& store, const std::wstring& text) {
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
        if (!store.DeleteKeyTree(currentKey)) {
          return false;
        }
      } else {
        if (!store.PutKey(currentKey)) {
          return false;
        }
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
      if (!store.DeleteValue(currentKey, valueName)) {
        return false;
      }
      continue;
    }
    if (right.size() >= 2 && right.front() == L'\"' && right.back() == L'\"') {
      std::wstring s = UnescapeRegString(right.substr(1, right.size() - 2));
      auto data = ParseData(REG_SZ, s);
      if (!store.PutValue(currentKey, valueName, REG_SZ, data.data(), (uint32_t)data.size())) {
        return false;
      }
      continue;
    }
    if (right.rfind(L"dword:", 0) == 0) {
      std::wstring hex = right.substr(6);
      uint32_t v = std::stoul(hex, nullptr, 16);
      std::vector<uint8_t> data(4);
      std::memcpy(data.data(), &v, 4);
      if (!store.PutValue(currentKey, valueName, REG_DWORD, data.data(), (uint32_t)data.size())) {
        return false;
      }
      continue;
    }
    if (right.rfind(L"hex:", 0) == 0) {
      std::wstring hex = right.substr(4);
      auto data = ParseData(REG_BINARY, hex);
      if (!store.PutValue(currentKey, valueName, REG_BINARY, data.data(), (uint32_t)data.size())) {
        return false;
      }
      continue;
    }
    if (right.rfind(L"hex(b):", 0) == 0) {
      std::wstring hex = right.substr(7);
      auto data = ParseData(REG_BINARY, hex);
      if (!store.PutValue(currentKey, valueName, REG_QWORD, data.data(), (uint32_t)data.size())) {
        return false;
      }
      continue;
    }

    // Generic .reg typed hex syntax: hex(<n>):<byte-csv>
    // Example: "0"=hex(0):   (REG_NONE with empty data)
    // Example: "X"=hex(2):01,00,00,00 (REG_EXPAND_SZ raw bytes)
    if (right.rfind(L"hex(", 0) == 0) {
      const size_t closeParen = right.find(L')', 4);
      if (closeParen != std::wstring::npos) {
        const size_t colon = (closeParen + 1 < right.size() && right[closeParen + 1] == L':') ? (closeParen + 1) : std::wstring::npos;
        if (colon != std::wstring::npos) {
          std::wstring typeText = Trim(right.substr(4, closeParen - 4));
          if (!typeText.empty()) {
            // The registry type is encoded as hex in the parentheses (e.g. hex(b) for 0xB).
            // Accept multi-digit hex too (e.g. hex(10) -> 0x10).
            uint32_t typeId = 0;
            try {
              typeId = (uint32_t)std::stoul(typeText, nullptr, 16);
            } catch (...) {
              typeId = 0;
            }

            std::wstring hex = Trim(right.substr(colon + 1));
            auto data = ParseData(REG_BINARY, hex);
            if (!store.PutValue(currentKey, valueName, typeId, data.data(), (uint32_t)data.size())) {
              return false;
            }
            continue;
          }
        }
      }
    }
  }

  return true;
}

} // namespace hklmwrap::regfile
