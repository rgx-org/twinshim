#include "shim/registry_hooks_utils.h"

#include <cstring>

namespace hklmwrap {

std::wstring CanonicalizeSubKey(const std::wstring& s) {
  std::wstring out = s;
  while (!out.empty() && (out.front() == L'\\' || out.front() == L'/')) {
    out.erase(out.begin());
  }
  while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) {
    out.pop_back();
  }
  for (auto& ch : out) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  return out;
}

std::wstring JoinKeyPath(const std::wstring& base, const std::wstring& sub) {
  if (sub.empty()) {
    return base;
  }
  if (base.empty()) {
    return sub;
  }
  if (base.back() == L'\\') {
    return base + sub;
  }
  return base + L"\\" + sub;
}

std::wstring AnsiToWide(const char* s, int len) {
  if (!s) {
    return {};
  }
  if (len == 0) {
    return {};
  }
  bool nullTerminatedInput = (len < 0);
  int needed = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out;
  out.resize((size_t)needed);
  MultiByteToWideChar(CP_ACP, 0, s, len, out.data(), needed);
  if (nullTerminatedInput && !out.empty() && out.back() == L'\0') {
    out.pop_back();
  }
  return out;
}

bool TryReadWideString(const wchar_t* s, std::wstring& out) {
  out.clear();
  if (!s) {
    return true;
  }
  if (IsBadStringPtrW(s, 32767)) {
    return false;
  }
  out.assign(s);
  return true;
}

bool TryAnsiToWideString(const char* s, std::wstring& out) {
  out.clear();
  if (!s) {
    return true;
  }
  if (IsBadStringPtrA(s, 32767)) {
    return false;
  }
  out = AnsiToWide(s, -1);
  return true;
}

std::wstring CaseFold(const std::wstring& s) {
  std::wstring out;
  out.resize(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    out[i] = (wchar_t)towlower(s[i]);
  }
  return out;
}

std::vector<uint8_t> EnsureWideStringData(DWORD type, const BYTE* data, DWORD cbData) {
  // Convert ANSI string payloads to UTF-16LE for storage.
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
    if (!data || !cbData) {
      return {};
    }
    return std::vector<uint8_t>(data, data + cbData);
  }

  if (!data || cbData == 0) {
    // Empty string: include terminator.
    std::wstring empty = L"";
    std::vector<uint8_t> out((empty.size() + 1) * sizeof(wchar_t));
    std::memcpy(out.data(), empty.c_str(), out.size());
    if (type == REG_MULTI_SZ) {
      // Double-NULL terminator.
      out.resize(2 * sizeof(wchar_t), 0);
    }
    return out;
  }

  const char* bytes = reinterpret_cast<const char*>(data);
  size_t srcLen = (size_t)cbData;
  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    size_t n = 0;
    while (n < srcLen && bytes[n] != '\0') {
      ++n;
    }
    srcLen = n;
  } else {
    // REG_MULTI_SZ: keep complete strings up through the first double-NUL terminator.
    size_t pos = 0;
    while ((pos + 1) < srcLen) {
      if (bytes[pos] == '\0' && bytes[pos + 1] == '\0') {
        srcLen = pos;
        break;
      }
      ++pos;
    }
  }

  std::wstring wide = AnsiToWide(bytes, (int)srcLen);
  wide.push_back(L'\0');
  if (type == REG_MULTI_SZ) {
    wide.push_back(L'\0');
  }
  std::vector<uint8_t> out(wide.size() * sizeof(wchar_t));
  std::memcpy(out.data(), wide.data(), out.size());
  return out;
}

std::vector<uint8_t> WideToAnsiBytesForQuery(DWORD type, const std::vector<uint8_t>& wideBytes) {
  if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) {
    return wideBytes;
  }
  if (wideBytes.empty()) {
    return std::vector<uint8_t>{0};
  }
  const wchar_t* w = reinterpret_cast<const wchar_t*>(wideBytes.data());
  int wchars = (int)(wideBytes.size() / sizeof(wchar_t));
  if (type == REG_SZ || type == REG_EXPAND_SZ) {
    int n = 0;
    while (n < wchars && w[n] != L'\0') {
      ++n;
    }
    wchars = n + 1;
  } else {
    // REG_MULTI_SZ: include data up through first double-NUL (or clamp to full buffer).
    int pos = 0;
    while ((pos + 1) < wchars) {
      if (w[pos] == L'\0' && w[pos + 1] == L'\0') {
        wchars = pos + 2;
        break;
      }
      ++pos;
    }
  }
  int needed = WideCharToMultiByte(CP_ACP, 0, w, wchars, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return std::vector<uint8_t>{0};
  }
  std::vector<uint8_t> out((size_t)needed);
  WideCharToMultiByte(CP_ACP, 0, w, wchars, reinterpret_cast<char*>(out.data()), needed, nullptr, nullptr);
  // Make sure we end with null (or double-null for multi_sz).
  if (out.empty() || out.back() != 0) {
    out.push_back(0);
  }
  if (type == REG_MULTI_SZ) {
    if (out.size() < 2 || out[out.size() - 2] != 0) {
      out.push_back(0);
    }
  }
  return out;
}

}
